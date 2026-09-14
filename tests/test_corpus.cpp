// Регрессия на эталонных записях: пары mic.wav + ref.wav из bomboec-rec (микрофон и loopback
// колонок, выровненные по QPC) прогоняются через настоящий Pipeline с измеренным дрейфом
// часов и цепочкой из конфига, а по сегментам записи (эхо, речь, double-talk, тишина, шум)
// считается, что приложение сделало с сигналом:
//   - echo (в колонках музыка, человек молчит): подавление mic -> out, dB, не меньше порога;
//   - double_talk (музыка и речь): подавление не меньше порога (речь остаётся, эхо уходит);
//   - speech (только речь): потеря уровня не больше порога, речь не задавлена;
//   - silence / noise (ничего или бытовой шум без музыки): выход тише входа не меньше чем на порог;
//   - на любом сегменте: нет всплесков (RMS за 100 ms на выходе не выше входа плюс порог).
// Дополнительно каждый сегмент сравнивается с базовым значением из манифеста: результат не
// должен ухудшиться больше чем на regression_tolerance_db. Так ловятся регрессии выравнивания,
// AEC3, NS и регулятора выхода на реальном звуке, а не только на синтетике.
//
// Манифест: tests/corpus/corpus.toml (формат описан там). Записи лежат вне git
// (recordings/, большие WAV); без них тест пропускается. Обновить базовые значения:
//   set BOMBOEC_CORPUS_BASELINE=1 && bomboec_tests "[corpus]"   (печатает строки для манифеста)
// Сравнить цепочки: BOMBOEC_CORPUS_CONFIG=<toml> подменяет конфиг (пороги и baseline тогда не
// проверяются, отчёт по каждому сценарию и смеси печатается). Послушать смеси:
// BOMBOEC_CORPUS_DUMP=<каталог> пишет туда вход, выход и чистый прогон каждой смеси.

#include "core/config.h"
#include "core/wav.h"
#include "sim/pipeline_sim.h"
#include "stages/builtin_stages.h"

#include <catch2/catch_test_macros.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace bomboec;
using namespace bomboec::test;

namespace {

namespace fs = std::filesystem;

struct Segment {
    std::string type;  // echo | double_talk | speech | silence | noise | transients
    double from = 0.0, to = 0.0;
    std::optional<double> baseline;
};

struct Scenario {
    std::string name;
    fs::path dir;
    std::vector<Segment> segments;
};

// Смесь: чистая речь плюс отдельно записанный шум (и/или эхо с его reference); выход
// сравнивается с чистой речью. Так видно, что обработка делает с голосом в присутствии помехи,
// чего средний RMS сегмента не показывает (гейт, режущий согласные, не менял RMS речи).
struct Mix {
    std::string name;
    fs::path speech;  // каталог сценария с чистой речью (mic.wav)
    fs::path noise;   // каталог с шумом без речи (mic.wav), пусто = без шума
    fs::path echo;    // каталог с эхом без речи (mic.wav + ref.wav), пусто = без reference
    fs::path render;  // каталог, чей ref.wav звучит в колонках, но в микрофон не попадает (без его mic.wav)
    std::vector<double> renderShiftsMs;  // прогоны со сдвигом reference, метрики усредняются; пусто = один без сдвига
    double noiseGainDb = 0.0, echoGainDb = 0.0;
    std::optional<double> baseLsd, baseDip, baseNoise;
};

struct Thresholds {
    double echoMinDb = 10.0;
    double doubleTalkMinDb = 3.0;
    double speechMaxLossDb = 3.0;
    double quietMinReductionDb = 4.0;
    double burstMaxDb = 6.0;
    double transientMinDb = 6.0;     // медиана подавления по событиям (стук, щелчок)
    double transientEventDb = 15.0;  // событие: кадр 10 ms громче медианы сегмента на столько
    double mixSpeechLsdMaxDb = 6.0;  // смесь: среднее спектральное расстояние выхода от чистой речи
    double mixDipMax = 0.05;         // смесь: доля речевых кадров, просевших > 6 dB против медианы
    double mixNoiseMinDb = 6.0;      // смесь: подавление на кадрах без речи
    double mixRenderMaxDb = 0.5;     // смесь с render: lsd против чистой речи через ту же цепочку
    double regressionToleranceDb = 1.5;
    double guardSec = 0.3;  // края сегмента не в счёт (переходы, задержка выхода)
};

struct Corpus {
    fs::path root;  // каталог записей
    std::string config;
    Thresholds th;
    std::vector<Mix> mixes;
    std::vector<Scenario> scenarios;
};

// Переменная окружения без getenv (MSVC считает его небезопасным).
std::string envVar(const char* name) {
    char buf[1024];
    size_t len = 0;
    if (getenv_s(&len, buf, sizeof(buf), name) != 0 || len == 0) return {};
    return std::string(buf, len - 1);
}

fs::path manifestPath() {
    const std::string env = envVar("BOMBOEC_CORPUS");
    if (!env.empty()) return fs::path(env);
    return fs::path(BOMBOEC_SOURCE_DIR) / "tests" / "corpus" / "corpus.toml";
}

bool loadCorpus(const fs::path& manifest, Corpus& out, std::string& error) {
    toml::table root;
    try {
        root = toml::parse_file(manifest.string());
    } catch (const toml::parse_error& e) {
        std::ostringstream oss;
        oss << e;
        error = oss.str();
        return false;
    }
    out.root = manifest.parent_path() / root["root"].value_or(std::string("."));
    out.root = out.root.lexically_normal();
    out.config = root["config"].value_or(std::string());
    if (const toml::table* t = root["thresholds"].as_table()) {
        out.th.echoMinDb = (*t)["echo_min_db"].value_or(out.th.echoMinDb);
        out.th.doubleTalkMinDb = (*t)["double_talk_min_db"].value_or(out.th.doubleTalkMinDb);
        out.th.speechMaxLossDb = (*t)["speech_max_loss_db"].value_or(out.th.speechMaxLossDb);
        out.th.quietMinReductionDb = (*t)["quiet_min_reduction_db"].value_or(out.th.quietMinReductionDb);
        out.th.burstMaxDb = (*t)["burst_max_db"].value_or(out.th.burstMaxDb);
        out.th.transientMinDb = (*t)["transient_min_db"].value_or(out.th.transientMinDb);
        out.th.transientEventDb = (*t)["transient_event_db"].value_or(out.th.transientEventDb);
        out.th.mixSpeechLsdMaxDb = (*t)["mix_speech_lsd_max_db"].value_or(out.th.mixSpeechLsdMaxDb);
        out.th.mixDipMax = (*t)["mix_dip_max"].value_or(out.th.mixDipMax);
        out.th.mixNoiseMinDb = (*t)["mix_noise_min_db"].value_or(out.th.mixNoiseMinDb);
        out.th.mixRenderMaxDb = (*t)["mix_render_max_db"].value_or(out.th.mixRenderMaxDb);
        out.th.regressionToleranceDb = (*t)["regression_tolerance_db"].value_or(out.th.regressionToleranceDb);
        out.th.guardSec = (*t)["guard_sec"].value_or(out.th.guardSec);
    }
    const toml::array* list = root["scenario"].as_array();
    if (!list) {
        error = "no [[scenario]] in " + manifest.string();
        return false;
    }
    for (const toml::node& node : *list) {
        const toml::table* t = node.as_table();
        if (!t) continue;
        Scenario s;
        s.name = (*t)["name"].value_or(std::string());
        s.dir = out.root / (*t)["dir"].value_or(std::string());
        if (s.name.empty()) s.name = (*t)["dir"].value_or(std::string());
        const toml::array* baseline = (*t)["baseline"].as_array();
        if (const toml::array* segs = (*t)["segments"].as_array()) {
            size_t k = 0;
            for (const toml::node& sn : *segs) {
                const toml::table* st = sn.as_table();
                if (!st) continue;
                Segment seg;
                seg.type = (*st)["type"].value_or(std::string());
                seg.from = (*st)["from"].value_or(0.0);
                seg.to = (*st)["to"].value_or(0.0);
                if (baseline && k < baseline->size()) seg.baseline = (*baseline)[k].value<double>();
                s.segments.push_back(seg);
                ++k;
            }
        }
        out.scenarios.push_back(std::move(s));
    }
    if (const toml::array* mixes = root["mix"].as_array()) {
        for (const toml::node& node : *mixes) {
            const toml::table* t = node.as_table();
            if (!t) continue;
            Mix m;
            m.name = (*t)["name"].value_or(std::string());
            m.speech = out.root / (*t)["speech"].value_or(std::string());
            if (const auto v = (*t)["noise"].value<std::string>()) m.noise = out.root / *v;
            if (const auto v = (*t)["echo"].value<std::string>()) m.echo = out.root / *v;
            if (const auto v = (*t)["render"].value<std::string>()) m.render = out.root / *v;
            if (!m.echo.empty() && !m.render.empty()) {
                error = "mix '" + m.name + "': echo and render both set the reference";
                return false;
            }
            if (const toml::array* shifts = (*t)["render_shifts_ms"].as_array()) {
                for (const toml::node& v : *shifts) m.renderShiftsMs.push_back(v.value_or(0.0));
                if (m.render.empty()) {
                    error = "mix '" + m.name + "': render_shifts_ms without render";
                    return false;
                }
            }
            m.noiseGainDb = (*t)["noise_gain_db"].value_or(0.0);
            m.echoGainDb = (*t)["echo_gain_db"].value_or(0.0);
            if (const toml::array* b = (*t)["baseline"].as_array()) {
                if (b->size() > 0) m.baseLsd = (*b)[0].value<double>();
                if (b->size() > 1) m.baseDip = (*b)[1].value<double>();
                if (b->size() > 2) m.baseNoise = (*b)[2].value<double>();
            }
            out.mixes.push_back(std::move(m));
        }
    }
    return true;
}

// "drift_ppm": X для дорожек mic и ref из metadata.json (без JSON-библиотеки).
void readDrift(const fs::path& metadata, double& micPpm, double& refPpm) {
    micPpm = refPpm = 0.0;
    const std::ifstream in(metadata);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    const std::regex rx("\"drift_ppm\":\\s*(-?[0-9.]+)");
    std::vector<double> values;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), rx); it != std::sregex_iterator(); ++it) {
        values.push_back(std::stod((*it)[1].str()));
    }
    if (values.size() >= 1) micPpm = values[0];
    if (values.size() >= 2) refPpm = values[1];
}

double rmsDb(const std::vector<float>& x, size_t from, size_t to) {
    to = std::min(to, x.size());
    if (from >= to) return -120.0;
    double e = 0.0;
    for (size_t i = from; i < to; ++i) e += double(x[i]) * x[i];
    return 10.0 * std::log10(e / double(to - from) + 1e-12);
}

// Задержка выхода относительно входа (кадры стадий, запас кольца, фаза чтения) с точностью до
// сэмпла. Грубо: максимум корреляции огибающих RMS по 10 ms при лаге 0..20 кадров. Точно:
// взаимная корреляция самих сигналов в пределах кадра от грубого лага, по самым громким кадрам
// входа (там сигнал на выходе есть и после NS). Выравнивание только до кадра засчитывало бы
// задержку стадии меньше кадра (lookahead гейта) как порчу голоса: спектры кадров расходятся.
// Без выравнивания щелчок на границе окна попадал бы в выход на окно позже и выглядел всплеском.
size_t estimateLagSamples(const std::vector<float>& in, const std::vector<float>& out, size_t frame) {
    const size_t n = std::min(in.size(), out.size()) / frame;
    if (n < 40) return 0;
    std::vector<double> em(n), eo(n);
    for (size_t k = 0; k < n; ++k) {
        em[k] = rmsDb(in, k * frame, (k + 1) * frame);
        eo[k] = rmsDb(out, k * frame, (k + 1) * frame);
    }
    size_t bestLag = 0;
    double best = -1e300;
    for (size_t lag = 0; lag <= 20; ++lag) {
        double sum = 0.0;
        for (size_t k = 0; k + lag < n; ++k) sum += em[k] * eo[k + lag];
        if (sum > best) {
            best = sum;
            bestLag = lag;
        }
    }

    constexpr size_t kLoudFrames = 300;
    std::vector<size_t> order(n - 22);  // кадры, у которых есть выход на любом лаге уточнения
    for (size_t k = 0; k < order.size(); ++k) order[k] = k + 1;
    const size_t keep = std::min(kLoudFrames, order.size());
    std::partial_sort(order.begin(), order.begin() + std::ptrdiff_t(keep), order.end(),
                      [&](size_t a, size_t b) { return em[a] > em[b]; });
    const size_t coarse = bestLag * frame;
    const size_t lo = coarse >= frame ? coarse - frame : 0;
    size_t lag = coarse;
    best = -1e300;
    for (size_t l = lo; l <= coarse + frame; ++l) {
        double sum = 0.0;
        for (size_t j = 0; j < keep; ++j) {
            const size_t a = order[j] * frame;
            for (size_t i = 0; i < frame; ++i) sum += double(in[a + i]) * out[a + i + l];
        }
        if (sum > best) {
            best = sum;
            lag = l;
        }
    }
    return lag;
}

// Максимум по окнам 100 ms превышения выхода над входом, dB.
double maxBurstDb(const std::vector<float>& mic, const std::vector<float>& out, size_t from, size_t to) {
    constexpr size_t kWindow = 4800;
    double worst = -120.0;
    for (size_t a = from; a + kWindow <= std::min({to, mic.size(), out.size()}); a += kWindow) {
        const double in = rmsDb(mic, a, a + kWindow);
        if (in < -60.0) continue;  // на тишине всплеск измеряют абсолютно ниже
        worst = std::max(worst, rmsDb(out, a, a + kWindow) - in);
    }
    return worst;
}

// Импульсные помехи (стук по столу, щелчок): средний RMS сегмента размывает их тишиной между
// ударами, поэтому метрика по событиям. Событие: кадры 10 ms, где микрофон громче медианы
// сегмента (шумовой пол) на eventDb; соседние кадры (разрыв <= 2) - одно событие. Подавление
// события: пик микрофона минус пик выхода (выход смотрится на кадр дальше: остаток лага).
struct TransientStats {
    size_t events = 0;
    double floorDb = -120.0;
    double medianAttenuationDb = 0.0;
    double minAttenuationDb = 0.0;
    double worstResidualDb = -120.0;  // самый громкий пик на выходе, dBFS
};

TransientStats transientStats(const std::vector<float>& mic, const std::vector<float>& out, size_t from, size_t to,
                              size_t frame, double eventDb) {
    TransientStats ts;
    to = std::min({to, mic.size(), out.size()});
    if (from + 2 * frame > to) return ts;
    const size_t n = (to - from) / frame;
    std::vector<double> em(n), eo(n);
    for (size_t k = 0; k < n; ++k) {
        em[k] = rmsDb(mic, from + k * frame, from + (k + 1) * frame);
        eo[k] = rmsDb(out, from + k * frame, from + (k + 1) * frame);
    }
    std::vector<double> sorted = em;
    std::sort(sorted.begin(), sorted.end());
    ts.floorDb = sorted[n / 2];
    const double gate = ts.floorDb + eventDb;
    std::vector<double> attenuation;
    for (size_t k = 0; k < n;) {
        if (em[k] < gate) {
            ++k;
            continue;
        }
        size_t end = k;  // последний кадр события
        for (size_t j = k + 1; j < n && j <= end + 3; ++j) {
            if (em[j] >= gate) end = j;
        }
        double peakMic = -120.0, peakOut = -120.0;
        for (size_t j = k; j <= std::min(end + 1, n - 1); ++j) {
            if (j <= end) peakMic = std::max(peakMic, em[j]);
            peakOut = std::max(peakOut, eo[j]);
        }
        attenuation.push_back(peakMic - peakOut);
        ts.worstResidualDb = std::max(ts.worstResidualDb, peakOut);
        k = end + 2;
    }
    ts.events = attenuation.size();
    if (ts.events == 0) return ts;
    std::sort(attenuation.begin(), attenuation.end());
    ts.minAttenuationDb = attenuation.front();
    ts.medianAttenuationDb = attenuation[attenuation.size() / 2];
    return ts;
}

struct SegmentResult {
    Segment seg;
    double micDb = 0.0, outDb = 0.0, attenuationDb = 0.0, burstDb = 0.0;
    std::optional<TransientStats> transients;  // только для сегментов transients
};

struct ScenarioResult {
    std::vector<SegmentResult> segments;
    double micPpm = 0.0, refPpm = 0.0;
    std::optional<double> erleDb, delayMs;
    uint64_t refMissing = 0, refJumps = 0, outTrimmed = 0;
    double outLagMs = 0.0;
};

bool loadChainConfig(const Corpus& corpus, AppConfig& cfg, std::string& error) {
    return parseConfig(corpus.config.empty() ? defaultConfigToml() : std::string_view(corpus.config), cfg, error);
}

// Моно микрофона (канал 0) из mic.wav сценария; частота должна совпадать с конвейером.
bool readMicMono(const fs::path& dir, uint32_t rate, std::vector<float>& mono, std::string& error) {
    std::vector<float> data;
    uint32_t ch = 0, fileRate = 0;
    if (!readWavFile(dir / "mic.wav", data, ch, fileRate, error)) return false;
    if (fileRate != rate) {
        error = "sample rate mismatch in " + (dir / "mic.wav").string();
        return false;
    }
    mono.resize(data.size() / ch);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = data[i * ch];
    return true;
}

// Прогон mic + ref через настоящий Pipeline с цепочкой конфига и измеренным дрейфом; выход в
// sim.output. refData interleaved с refCh каналами, пусто = reference молчит.
bool runChain(const AppConfig& cfg, const std::vector<float>& mic, const std::vector<float>& refData, uint32_t refCh,
              double micPpm, double refPpm, PipelineSim& sim, std::string& error) {
    sim.fmt = cfg.format;
    sim.settings = cfg.engine;
    sim.settings.recordDir.clear();
    sim.mic.ppm = micPpm;
    sim.ref.ppm = refPpm;
    sim.mic.jitterMs = 0.2;
    sim.ref.jitterMs = 0.03;
    sim.micSignal = [&](uint64_t i, uint32_t) { return i < mic.size() ? mic[i] : 0.0f; };
    const size_t refFrames = refCh ? refData.size() / refCh : 0;
    sim.refSignal = [&](uint64_t i, uint32_t c) {
        return i < refFrames ? refData[i * refCh + std::min<uint32_t>(c, refCh - 1)] : 0.0f;
    };
    StageRegistry reg;
    registerBuiltinStages(reg);
    std::unique_ptr<Chain> chain = buildChain(reg, cfg, error);
    if (!chain) return false;
    if (!sim.start(std::move(chain))) {
        error = "pipeline start failed";
        return false;
    }
    sim.run(double(mic.size()) / cfg.format.sampleRate + 0.5);
    sim.micSignal = nullptr;
    sim.refSignal = nullptr;
    return true;
}

// Лаг выхода для сценариев: оценивается на записи чистой речи первой смеси, которую цепочка
// пропускает, и запоминается на конфиг. На самих сценариях корреляции часто не за что
// зацепиться: тишина и эхо без речи на выходе почти нули, а удары, которые убрал гейт, уводили
// оценку на 9 ms. Лаг задаёт движок и стадии, от записи он зависит на пару сэмплов.
std::optional<size_t> chainLagSamples(const Corpus& corpus, const AppConfig& cfg) {
    static std::string cachedConfig;
    static std::optional<size_t> cached;
    const std::string key = corpus.config.empty() ? std::string(defaultConfigToml()) : corpus.config;
    if (cached && cachedConfig == key) return cached;
    for (const Mix& m : corpus.mixes) {
        std::vector<float> speech;
        std::string error;
        if (!fs::exists(m.speech / "mic.wav") || !readMicMono(m.speech, cfg.format.sampleRate, speech, error)) continue;
        speech.resize(std::min(speech.size(), size_t(10 * cfg.format.sampleRate)));
        double micPpm = 0.0, refPpm = 0.0;
        readDrift(m.speech / "metadata.json", micPpm, refPpm);
        PipelineSim sim;
        if (!runChain(cfg, speech, {}, 0, micPpm, refPpm, sim, error)) return std::nullopt;
        cached = estimateLagSamples(speech, sim.output, cfg.format.frameSamples);
        cachedConfig = key;
        return cached;
    }
    return std::nullopt;
}

bool runScenario(const Corpus& corpus, const Scenario& sc, ScenarioResult& r, std::string& error) {
    AppConfig cfg;
    if (!loadChainConfig(corpus, cfg, error)) return false;
    std::vector<float> mic, refData;
    uint32_t refCh = 0, refRate = 0;
    if (!readMicMono(sc.dir, cfg.format.sampleRate, mic, error) ||
        !readWavFile(sc.dir / "ref.wav", refData, refCh, refRate, error)) {
        return false;
    }
    if (refRate != cfg.format.sampleRate) {
        error = "sample rate mismatch";
        return false;
    }
    readDrift(sc.dir / "metadata.json", r.micPpm, r.refPpm);
    PipelineSim sim;
    if (!runChain(cfg, mic, refData, refCh, r.micPpm, r.refPpm, sim, error)) return false;

    const double rate = cfg.format.sampleRate;
    // Выход сдвинут на задержку конвейера: сравниваем одно и то же время.
    const std::optional<size_t> chainLag = chainLagSamples(corpus, cfg);
    const size_t lag = chainLag ? *chainLag : estimateLagSamples(mic, sim.output, cfg.format.frameSamples);
    r.outLagMs = double(lag) * 1000.0 / rate;
    const std::vector<float> out(sim.output.begin() + std::ptrdiff_t(std::min(lag, sim.output.size())),
                                 sim.output.end());
    for (const Segment& seg : sc.segments) {
        SegmentResult sr;
        sr.seg = seg;
        const auto from = size_t((seg.from + corpus.th.guardSec) * rate);
        const auto to = size_t((seg.to - corpus.th.guardSec) * rate);
        sr.micDb = rmsDb(mic, from, to);
        sr.outDb = rmsDb(out, from, to);
        sr.attenuationDb = sr.micDb - sr.outDb;
        sr.burstDb = maxBurstDb(mic, out, from, to);
        if (seg.type == "transients") {
            sr.transients = transientStats(mic, out, from, to, cfg.format.frameSamples, corpus.th.transientEventDb);
            sr.attenuationDb = sr.transients->medianAttenuationDb;  // baseline и регрессия по медиане событий
        }
        r.segments.push_back(sr);
    }
    const StageStats st = sim.pipeline.chainStats();
    r.erleDb = st.erleDb;
    r.delayMs = st.delayMs;
    const PipelineStats ps = sim.pipeline.stats();
    r.refMissing = ps.refMissing;
    r.refJumps = ps.refJumps;
    r.outTrimmed = ps.outTrimmed;
    return true;
}

// --- смеси -----------------------------------------------------------------------------------

// Спектр мощности кадра в 24 полосах (логарифмически от 100 Hz до 12 kHz), dB. Кадр 480 сэмплов
// дополняется нулями до 512, окно Ханна, ДПФ по определению на нужных бинах (тест, не realtime).
constexpr size_t kBands = 24;

std::array<double, kBands> bandSpectrumDb(const float* x, size_t n, double rate) {
    constexpr size_t kFft = 512;
    static std::vector<double> window;
    static std::vector<std::pair<size_t, size_t>> bins;  // [from, to) бинов на полосу
    if (window.size() != n) {
        window.resize(n);
        for (size_t i = 0; i < n; ++i) window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * double(i) / double(n));
        bins.clear();
        const double hzPerBin = rate / kFft;
        for (size_t b = 0; b < kBands; ++b) {
            const double lo = 100.0 * std::pow(120.0, double(b) / kBands);
            const double hi = 100.0 * std::pow(120.0, double(b + 1) / kBands);
            bins.emplace_back(
                size_t(std::floor(lo / hzPerBin)),
                std::max<size_t>(size_t(std::floor(hi / hzPerBin)), size_t(std::floor(lo / hzPerBin)) + 1));
        }
    }
    std::array<double, kBands> out{};
    for (size_t b = 0; b < kBands; ++b) {
        double e = 0.0;
        for (size_t k = bins[b].first; k < bins[b].second; ++k) {
            double re = 0.0, im = 0.0;
            for (size_t i = 0; i < n; ++i) {
                const double ph = -2.0 * std::numbers::pi * double(k) * double(i) / kFft;
                const double v = double(x[i]) * window[i];
                re += v * std::cos(ph);
                im += v * std::sin(ph);
            }
            e += re * re + im * im;
        }
        out[b] = 10.0 * std::log10(e / double(bins[b].second - bins[b].first) + 1e-12);
    }
    return out;
}

struct MixResult {
    size_t speechFrames = 0, noiseFrames = 0;
    double lsdDb = 0.0;             // среднее |out - clean| по полосам на речевых кадрах, dB
    double lsdVsCleanOut = 0.0;     // то же против чистой речи, прошедшей ту же цепочку без помехи
    double gainMedianDb = 0.0;      // медиана out - clean по уровню речевых кадров
    double dipFrac = 0.0;           // доля речевых кадров с уровнем ниже медианы на 6 dB и больше
    double noiseReductionDb = 0.0;  // на кадрах без речи: энергия входа смеси к энергии выхода
    double residualDb = -120.0;     // самый громкий кадр выхода без речи, dBFS (остаток удара)
    double snrInDb = 0.0;           // речь / помеха во входной смеси
    double outLagMs = 0.0;
    uint64_t transients = 0;
};

// Помеха с усилением, длиннее речи не нужна, короче - дополняется тишиной.
void addScaled(std::vector<float>& mix, const std::vector<float>& src, double gainDb) {
    const float g = float(std::pow(10.0, gainDb / 20.0));
    for (size_t i = 0; i < std::min(mix.size(), src.size()); ++i) mix[i] += g * src[i];
}

bool writeMono(const fs::path& path, const std::vector<float>& data, uint32_t rate, std::string& error) {
    WavWriter w;
    if (!w.open(path, 1, rate, error)) return false;
    w.write(data.data(), data.size());
    return true;
}

// Метрики выхода одной смеси: out (уже выровнен) против чистой речи clean и против её прогона
// через ту же цепочку cleanOut.
bool mixMetrics(const Corpus& corpus, const Mix& m, const std::vector<float>& clean, const std::vector<float>& mix,
                const std::vector<float>& out, const std::vector<float>& cleanOut, uint32_t rate, size_t frame,
                MixResult& r, std::string& error) {
    const size_t frames = std::min({clean.size(), out.size(), cleanOut.size()}) / frame;
    std::vector<double> cleanDb(frames), outDb(frames), mixDb(frames);
    for (size_t k = 0; k < frames; ++k) {
        cleanDb[k] = rmsDb(clean, k * frame, (k + 1) * frame);
        outDb[k] = rmsDb(out, k * frame, (k + 1) * frame);
        mixDb[k] = rmsDb(mix, k * frame, (k + 1) * frame);
    }
    // Пол шума чистой записи: 10-й процентиль уровней кадров (при непрерывном чтении медиана
    // сама речь). Речь: кадры громче пола на 15 dB, тишина: не громче чем на 3 dB.
    std::vector<double> sorted = cleanDb;
    std::sort(sorted.begin(), sorted.end());
    const double floor = sorted[frames / 10];
    const double speechGate = floor + 15.0, quietGate = floor + 3.0;
    const double guard = corpus.th.guardSec * rate / frame;

    std::vector<double> levelDiff;
    double lsdSum = 0.0, lsdCleanSum = 0.0, quietInE = 0.0, quietOutE = 0.0, residualPeak = -120.0, speechE = 0.0,
           noiseE = 0.0;
    size_t noiseFrames = 0;
    for (size_t k = size_t(guard); k + size_t(guard) < frames; ++k) {
        if (cleanDb[k] >= speechGate) {
            levelDiff.push_back(outDb[k] - cleanDb[k]);
            const auto sc = bandSpectrumDb(clean.data() + k * frame, frame, rate);
            const auto so = bandSpectrumDb(out.data() + k * frame, frame, rate);
            const auto sco = bandSpectrumDb(cleanOut.data() + k * frame, frame, rate);
            const double top = *std::max_element(sc.begin(), sc.end());
            double d = 0.0, dc = 0.0;
            size_t used = 0;
            for (size_t b = 0; b < kBands; ++b) {
                if (sc[b] < top - 40.0) continue;  // полосы без речи не в счёт
                d += std::fabs(so[b] - sc[b]);
                dc += std::fabs(so[b] - sco[b]);
                ++used;
            }
            lsdSum += d / double(std::max<size_t>(used, 1));
            lsdCleanSum += dc / double(std::max<size_t>(used, 1));
            speechE += std::pow(10.0, cleanDb[k] / 10.0);
            noiseE += std::pow(10.0, mixDb[k] / 10.0) - std::pow(10.0, cleanDb[k] / 10.0);
        } else if (cleanDb[k] < quietGate) {
            quietInE += std::pow(10.0, mixDb[k] / 10.0);
            quietOutE += std::pow(10.0, outDb[k] / 10.0);
            residualPeak = std::max(residualPeak, outDb[k]);
            ++noiseFrames;
        }
    }
    r.speechFrames = levelDiff.size();
    r.noiseFrames = noiseFrames;
    if (r.speechFrames == 0) {
        error = "no speech frames in " + m.speech.string();
        return false;
    }
    r.lsdDb = lsdSum / double(r.speechFrames);
    r.lsdVsCleanOut = lsdCleanSum / double(r.speechFrames);
    std::vector<double> ld = levelDiff;
    std::sort(ld.begin(), ld.end());
    r.gainMedianDb = ld[ld.size() / 2];
    r.dipFrac = double(std::count_if(ld.begin(), ld.end(), [&](double v) { return v < r.gainMedianDb - 6.0; })) /
                double(ld.size());
    r.noiseReductionDb = noiseFrames ? 10.0 * std::log10(quietInE / std::max(quietOutE, 1e-18)) : 0.0;
    r.residualDb = residualPeak;
    r.snrInDb = 10.0 * std::log10(speechE / std::max(noiseE, 1e-12));
    return true;
}

// Выход симуляции без задержки цепочки, оцененной по чистой речи.
std::vector<float> alignedOutput(const std::vector<float>& clean, const std::vector<float>& output, size_t frame,
                                 size_t& lag) {
    lag = estimateLagSamples(clean, output, frame);
    return {output.begin() + std::ptrdiff_t(std::min(lag, output.size())), output.end()};
}

// Reference позже микрофона на shift сэмплов (отрицательный - раньше).
std::vector<float> shiftedReference(const std::vector<float>& refData, uint32_t refCh, int64_t shift) {
    if (shift == 0 || refCh == 0) return refData;
    const size_t n = size_t(std::llabs(shift)) * refCh;
    if (shift > 0) {
        std::vector<float> out(n, 0.0f);
        out.insert(out.end(), refData.begin(), refData.end());
        return out;
    }
    return {refData.begin() + std::ptrdiff_t(std::min(n, refData.size())), refData.end()};
}

bool runMix(const Corpus& corpus, const Mix& m, MixResult& r, std::string& error) {
    AppConfig cfg;
    if (!loadChainConfig(corpus, cfg, error)) return false;
    const uint32_t rate = cfg.format.sampleRate;
    const size_t frame = cfg.format.frameSamples;
    std::vector<float> clean, noise, echoMic, refData;
    uint32_t refCh = 0;
    if (!readMicMono(m.speech, rate, clean, error)) return false;
    if (!m.noise.empty() && !readMicMono(m.noise, rate, noise, error)) return false;
    double micPpm = 0.0, refPpm = 0.0;
    readDrift(m.speech / "metadata.json", micPpm, refPpm);
    if (!m.echo.empty() && !readMicMono(m.echo, rate, echoMic, error)) return false;
    if (const fs::path& refDir = m.echo.empty() ? m.render : m.echo; !refDir.empty()) {
        uint32_t refRate = 0;
        if (!readWavFile(refDir / "ref.wav", refData, refCh, refRate, error)) return false;
        if (refRate != rate) {
            error = "sample rate mismatch in " + (refDir / "ref.wav").string();
            return false;
        }
        readDrift(refDir / "metadata.json", micPpm, refPpm);  // пара mic/ref записи согласована по дрейфу
    }
    std::vector<float> mix = clean;
    addScaled(mix, noise, m.noiseGainDb);
    addScaled(mix, echoMic, m.echoGainDb);

    PipelineSim simClean;
    if (!runChain(cfg, clean, {}, 0, micPpm, refPpm, simClean, error)) return false;
    size_t lagClean = 0;
    const std::vector<float> cleanOut = alignedOutput(clean, simClean.output, frame, lagClean);
    const std::string dump = envVar("BOMBOEC_CORPUS_DUMP");
    if (!dump.empty() && (!writeMono(fs::path(dump) / (m.name + ".in.wav"), mix, rate, error) ||
                          !writeMono(fs::path(dump) / (m.name + ".clean-out.wav"), simClean.output, rate, error))) {
        return false;
    }

    // Каждый сдвиг reference - отдельный прогон смеси, метрики усредняются (остаток - худший).
    const std::vector<double> shifts = m.renderShiftsMs.empty() ? std::vector<double>{0.0} : m.renderShiftsMs;
    r = MixResult{};
    r.residualDb = -120.0;
    for (const double shiftMs : shifts) {
        const std::vector<float> ref = shiftedReference(refData, refCh, int64_t(std::llround(shiftMs * rate / 1000.0)));
        PipelineSim simMix;
        if (!runChain(cfg, mix, ref, refCh, micPpm, refPpm, simMix, error)) return false;
        if (!dump.empty()) {
            std::ostringstream name;
            name << m.name << (shifts.size() > 1 ? ".out" + std::to_string(std::lround(shiftMs)) + "ms" : ".out")
                 << ".wav";
            if (!writeMono(fs::path(dump) / name.str(), simMix.output, rate, error)) return false;
        }
        // Выход смеси сверяется с чистой речью: помеху цепочка убирает, речь в выходе остаётся.
        size_t lag = 0;
        const std::vector<float> out = alignedOutput(clean, simMix.output, frame, lag);
        MixResult one;
        if (!mixMetrics(corpus, m, clean, mix, out, cleanOut, rate, frame, one, error)) return false;
        const double w = 1.0 / double(shifts.size());
        r.speechFrames = one.speechFrames;
        r.noiseFrames = one.noiseFrames;
        r.lsdDb += w * one.lsdDb;
        r.lsdVsCleanOut += w * one.lsdVsCleanOut;
        r.gainMedianDb += w * one.gainMedianDb;
        r.dipFrac += w * one.dipFrac;
        r.noiseReductionDb += w * one.noiseReductionDb;
        r.residualDb = std::max(r.residualDb, one.residualDb);
        r.snrInDb = one.snrInDb;
        r.outLagMs += w * double(lag) * 1000.0 / rate;
        r.transients += simMix.pipeline.chainStats().transients;
    }
    return true;
}

}  // namespace

TEST_CASE("Corpus: recorded scenarios keep their echo, speech and noise metrics", "[corpus]") {
    const fs::path manifest = manifestPath();
    if (!fs::exists(manifest)) SKIP("corpus manifest not found: " << manifest.string());
    Corpus corpus;
    std::string error;
    REQUIRE(loadCorpus(manifest, corpus, error));
    const bool printBaseline = !envVar("BOMBOEC_CORPUS_BASELINE").empty();
    const std::string configOverride = envVar("BOMBOEC_CORPUS_CONFIG");
    if (!configOverride.empty()) {
        std::ifstream in(configOverride);
        REQUIRE(in);
        std::stringstream ss;
        ss << in.rdbuf();
        corpus.config = ss.str();
        WARN("corpus: chain config from " << configOverride << ", thresholds not enforced");
    }
    size_t ran = 0;

    for (const Scenario& sc : corpus.scenarios) {
        if (!fs::exists(sc.dir / "mic.wav")) {
            WARN("corpus: scenario '" << sc.name << "' skipped, no recording in " << sc.dir.string());
            continue;
        }
        DYNAMIC_SECTION("scenario " << sc.name) {
            ScenarioResult r;
            REQUIRE(runScenario(corpus, sc, r, error));
            ++ran;
            std::ostringstream report;
            report << "scenario " << sc.name << ": drift mic " << r.micPpm << " / ref " << r.refPpm << " ppm, erle "
                   << (r.erleDb ? *r.erleDb : -1.0) << " dB, delay " << (r.delayMs ? *r.delayMs : -1.0)
                   << " ms, refMissing " << r.refMissing << ", trimmed " << r.outTrimmed << ", output lag "
                   << r.outLagMs << " ms\n";
            std::ostringstream baseline;
            baseline << "baseline = [";
            for (size_t k = 0; k < r.segments.size(); ++k) {
                const SegmentResult& s = r.segments[k];
                report << "  " << s.seg.type << " " << s.seg.from << "-" << s.seg.to << " s: mic " << s.micDb
                       << " -> out " << s.outDb << " dBFS, attenuation " << s.attenuationDb << " dB, burst "
                       << s.burstDb << " dB";
                if (s.transients) {
                    report << "; events " << s.transients->events << " over floor " << s.transients->floorDb
                           << " dBFS, attenuation median " << s.transients->medianAttenuationDb << " / min "
                           << s.transients->minAttenuationDb << " dB, worst residual " << s.transients->worstResidualDb
                           << " dBFS";
                }
                if (s.seg.baseline) report << " (baseline " << *s.seg.baseline << ")";
                report << "\n";
                baseline << (k ? ", " : "") << std::round(s.attenuationDb * 10.0) / 10.0;
            }
            baseline << "]";
            INFO(report.str());
            if (!configOverride.empty()) WARN(report.str());
            if (printBaseline || !configOverride.empty()) WARN("corpus " << sc.name << ": " << baseline.str());
            if (!configOverride.empty()) continue;
            const Thresholds& th = corpus.th;
            for (const SegmentResult& s : r.segments) {
                INFO("segment " << s.seg.type << " " << s.seg.from << "-" << s.seg.to << " s");
                const bool quiet = s.seg.type == "silence" || s.seg.type == "noise";
                if (s.seg.type == "echo") CHECK(s.attenuationDb >= th.echoMinDb);
                else if (s.seg.type == "double_talk") CHECK(s.attenuationDb >= th.doubleTalkMinDb);
                else if (s.seg.type == "speech") CHECK(s.attenuationDb <= th.speechMaxLossDb);
                else if (quiet) CHECK(s.attenuationDb >= th.quietMinReductionDb);
                else if (s.transients.has_value()) {
                    CHECK(s.transients->events >= 5);  // в записи должны быть удары
                    CHECK(s.attenuationDb >= th.transientMinDb);
                }
                CHECK(s.burstDb <= th.burstMaxDb);
                if (s.seg.baseline) {
                    // Речь: хуже = больше потеря; остальное: хуже = меньше подавление.
                    if (s.seg.type == "speech") CHECK(s.attenuationDb <= *s.seg.baseline + th.regressionToleranceDb);
                    else CHECK(s.attenuationDb >= *s.seg.baseline - th.regressionToleranceDb);
                }
            }
        }
    }
    if (ran == 0) SKIP("corpus: no recordings present under " << corpus.root.string());
}

TEST_CASE("Corpus mixes: clean speech plus a recorded disturbance stays close to the clean speech", "[corpus]") {
    const fs::path manifest = manifestPath();
    if (!fs::exists(manifest)) SKIP("corpus manifest not found: " << manifest.string());
    Corpus corpus;
    std::string error;
    REQUIRE(loadCorpus(manifest, corpus, error));
    const bool printBaseline = !envVar("BOMBOEC_CORPUS_BASELINE").empty();
    const std::string configOverride = envVar("BOMBOEC_CORPUS_CONFIG");
    if (!configOverride.empty()) {
        std::ifstream in(configOverride);
        REQUIRE(in);
        std::stringstream ss;
        ss << in.rdbuf();
        corpus.config = ss.str();
    }
    size_t ran = 0;
    for (const Mix& m : corpus.mixes) {
        const bool present = fs::exists(m.speech / "mic.wav") && (m.noise.empty() || fs::exists(m.noise / "mic.wav")) &&
                             (m.echo.empty() || fs::exists(m.echo / "ref.wav")) &&
                             (m.render.empty() || fs::exists(m.render / "ref.wav"));
        if (!present) {
            WARN("corpus: mix '" << m.name << "' skipped, recordings missing");
            continue;
        }
        DYNAMIC_SECTION("mix " << m.name) {
            MixResult r;
            REQUIRE(runMix(corpus, m, r, error));
            ++ran;
            std::ostringstream report;
            report << "mix " << m.name << ": snr in " << r.snrInDb << " dB, speech frames " << r.speechFrames
                   << ", noise frames " << r.noiseFrames << ", output lag " << r.outLagMs << " ms, transients "
                   << r.transients << "\n  speech: lsd " << r.lsdDb << " dB vs clean, " << r.lsdVsCleanOut
                   << " dB vs clean through the chain; level median " << r.gainMedianDb << " dB, dips "
                   << r.dipFrac * 100.0 << " %\n  noise: reduction " << r.noiseReductionDb << " dB, residual "
                   << r.residualDb << " dBFS peak";
            if (m.baseLsd)
                report << "\n  baseline lsd " << *m.baseLsd << ", dips " << *m.baseDip << ", noise " << *m.baseNoise;
            INFO(report.str());
            std::ostringstream baseline;
            baseline << "baseline = [" << std::round(r.lsdDb * 10.0) / 10.0 << ", "
                     << std::round(r.dipFrac * 1000.0) / 1000.0 << ", " << std::round(r.noiseReductionDb * 10.0) / 10.0
                     << "]";
            if (!configOverride.empty()) WARN(report.str());
            if (printBaseline || !configOverride.empty()) WARN("corpus mix " << m.name << ": " << baseline.str());
            if (!configOverride.empty()) continue;
            const Thresholds& th = corpus.th;
            CHECK(r.lsdDb <= th.mixSpeechLsdMaxDb);
            CHECK(r.dipFrac <= th.mixDipMax);
            if (!m.noise.empty() || !m.echo.empty()) CHECK(r.noiseReductionDb >= th.mixNoiseMinDb);
            // Звук, которого микрофон не слышит, не должен менять выход.
            if (!m.render.empty()) CHECK(r.lsdVsCleanOut <= th.mixRenderMaxDb);
            if (m.baseLsd) CHECK(r.lsdDb <= *m.baseLsd + th.regressionToleranceDb);
            if (m.baseDip) CHECK(r.dipFrac <= *m.baseDip + 0.02);
            if (m.baseNoise) CHECK(r.noiseReductionDb >= *m.baseNoise - th.regressionToleranceDb);
        }
    }
    if (ran == 0) SKIP("corpus: no mix recordings present under " << corpus.root.string());
}
