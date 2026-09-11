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

#include "core/config.h"
#include "core/wav.h"
#include "sim/pipeline_sim.h"
#include "stages/builtin_stages.h"

#include <catch2/catch_test_macros.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace bomboec;
using namespace bomboec::test;

namespace {

namespace fs = std::filesystem;

struct Segment {
    std::string type;  // echo | double_talk | speech | silence | noise
    double from = 0.0, to = 0.0;
    std::optional<double> baseline;
};

struct Scenario {
    std::string name;
    fs::path dir;
    std::vector<Segment> segments;
};

struct Thresholds {
    double echoMinDb = 10.0;
    double doubleTalkMinDb = 3.0;
    double speechMaxLossDb = 3.0;
    double quietMinReductionDb = 4.0;
    double burstMaxDb = 6.0;
    double regressionToleranceDb = 1.5;
    double guardSec = 0.3;  // края сегмента не в счёт (переходы, задержка выхода)
};

struct Corpus {
    fs::path root;  // каталог записей
    std::string config;
    Thresholds th;
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

// Максимум по окнам 100 ms превышения выхода над входом, dB.
// Задержка выхода относительно микрофона (кадр, запас кольца, фаза чтения): максимум
// корреляции огибающих RMS по 10 ms при лаге 0..20 кадров. Без выравнивания щелчок на границе
// окна попадал бы в выход на окно позже и выглядел всплеском.
size_t estimateLagSamples(const std::vector<float>& mic, const std::vector<float>& out, size_t frame) {
    const size_t n = std::min(mic.size(), out.size()) / frame;
    if (n < 40) return 0;
    std::vector<double> em(n), eo(n);
    for (size_t k = 0; k < n; ++k) {
        em[k] = rmsDb(mic, k * frame, (k + 1) * frame);
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
    return bestLag * frame;
}

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

struct SegmentResult {
    Segment seg;
    double micDb = 0.0, outDb = 0.0, attenuationDb = 0.0, burstDb = 0.0;
};

struct ScenarioResult {
    std::vector<SegmentResult> segments;
    double micPpm = 0.0, refPpm = 0.0;
    std::optional<double> erleDb, delayMs;
    uint64_t refMissing = 0, refJumps = 0, outTrimmed = 0;
    double outLagMs = 0.0;
};

bool runScenario(const Corpus& corpus, const Scenario& sc, ScenarioResult& r, std::string& error) {
    std::vector<float> micData, refData;
    uint32_t micCh = 0, refCh = 0, micRate = 0, refRate = 0;
    if (!readWavFile(sc.dir / "mic.wav", micData, micCh, micRate, error) ||
        !readWavFile(sc.dir / "ref.wav", refData, refCh, refRate, error)) {
        return false;
    }
    AppConfig cfg;
    if (!parseConfig(corpus.config.empty() ? defaultConfigToml() : std::string_view(corpus.config), cfg, error)) {
        return false;
    }
    if (micRate != cfg.format.sampleRate || refRate != cfg.format.sampleRate) {
        error = "sample rate mismatch";
        return false;
    }
    std::vector<float> mic(micData.size() / micCh);
    for (size_t i = 0; i < mic.size(); ++i) mic[i] = micData[i * micCh];
    readDrift(sc.dir / "metadata.json", r.micPpm, r.refPpm);

    PipelineSim sim;
    sim.fmt = cfg.format;
    sim.settings = cfg.engine;
    sim.settings.recordDir.clear();
    sim.mic.ppm = r.micPpm;
    sim.ref.ppm = r.refPpm;
    sim.mic.jitterMs = 0.2;
    sim.ref.jitterMs = 0.03;
    sim.micSignal = [&](uint64_t i, uint32_t) { return i < mic.size() ? mic[i] : 0.0f; };
    const size_t refFrames = refData.size() / refCh;
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

    const double rate = cfg.format.sampleRate;
    // Выход сдвинут на задержку конвейера: сравниваем одно и то же время.
    const size_t lag = estimateLagSamples(mic, sim.output, cfg.format.frameSamples);
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
        r.segments.push_back(sr);
    }
    const StageStats st = sim.pipeline.chainStats();
    r.erleDb = st.erleDb;
    r.delayMs = st.delayMs;
    const PipelineStats ps = sim.pipeline.stats();
    r.refMissing = ps.refMissing;
    r.refJumps = ps.refJumps;
    r.outTrimmed = ps.outTrimmed;
    sim.micSignal = nullptr;
    sim.refSignal = nullptr;
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
                if (s.seg.baseline) report << " (baseline " << *s.seg.baseline << ")";
                report << "\n";
                baseline << (k ? ", " : "") << std::round(s.attenuationDb * 10.0) / 10.0;
            }
            baseline << "]";
            INFO(report.str());
            if (printBaseline) WARN("corpus " << sc.name << ": " << baseline.str());
            const Thresholds& th = corpus.th;
            for (const SegmentResult& s : r.segments) {
                INFO("segment " << s.seg.type << " " << s.seg.from << "-" << s.seg.to << " s");
                const bool quiet = s.seg.type == "silence" || s.seg.type == "noise";
                if (s.seg.type == "echo") CHECK(s.attenuationDb >= th.echoMinDb);
                else if (s.seg.type == "double_talk") CHECK(s.attenuationDb >= th.doubleTalkMinDb);
                else if (s.seg.type == "speech") CHECK(s.attenuationDb <= th.speechMaxLossDb);
                else if (quiet) CHECK(s.attenuationDb >= th.quietMinReductionDb);
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
