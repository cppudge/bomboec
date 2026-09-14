#include "core/chain.h"
#include "stages/builtin_stages.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace bomboec;
using Catch::Approx;

namespace {

double rms(std::span<const float> x) {
    double e = 0.0;
    for (const float v : x) e += double(v) * v;
    return std::sqrt(e / double(x.size()));
}

// Прогоняет синус через стадию несколько кадров и возвращает RMS последнего кадра.
double toneResponse(IStage& stage, const PipelineFormat& fmt, double hz, int frames = 50) {
    Frame mic(1, fmt.frameSamples);
    double rmsOut = 0.0;
    for (int f = 0; f < frames; ++f) {
        for (uint32_t i = 0; i < fmt.frameSamples; ++i) {
            const double t = double(f * fmt.frameSamples + i) / fmt.sampleRate;
            mic.channel(0)[i] = float(0.5 * std::sin(2.0 * std::numbers::pi * hz * t));
        }
        stage.process(mic, nullptr);
        rmsOut = rms(mic.channel(0));
    }
    return rmsOut;
}

}  // namespace

TEST_CASE("HPF stage attenuates 20 Hz and passes 1 kHz") {
    const PipelineFormat fmt;
    auto hpf = makeHpfStage();
    toml::table cfg;
    cfg.insert("cutoff_hz", 80.0);
    std::string error;
    StageParams params(cfg);
    REQUIRE(hpf->init(fmt, params, error));
    CHECK(hpf->info().caps == capBit(Cap::Hpf));

    const double ref = 0.5 / std::numbers::sqrt2;
    const double low = toneResponse(*hpf, fmt, 20.0);
    hpf->reset();
    const double high = toneResponse(*hpf, fmt, 1000.0);
    CHECK(20.0 * std::log10(low / ref) < -20.0);
    CHECK(20.0 * std::log10(high / ref) == Approx(0.0).margin(0.2));

    toml::table bad;
    bad.insert("cutoff_hz", 30000.0);
    StageParams badParams(bad);
    CHECK_FALSE(hpf->init(fmt, badParams, error));
}

TEST_CASE("Limiter stage keeps peaks under the ceiling") {
    const PipelineFormat fmt;
    auto lim = makeLimiterStage();
    toml::table cfg;
    cfg.insert("ceiling_db", -6.0);
    std::string error;
    StageParams params(cfg);
    REQUIRE(lim->init(fmt, params, error));

    Frame mic(1, fmt.frameSamples);
    for (uint32_t i = 0; i < fmt.frameSamples; ++i) {
        mic.channel(0)[i] = (i % 2 == 0) ? 1.0f : -1.0f;
    }
    lim->process(mic, nullptr);
    const float ceiling = float(std::pow(10.0, -6.0 / 20.0));
    for (const float v : mic.channel(0)) {
        CHECK(std::fabs(v) <= ceiling + 1e-6f);
    }
    REQUIRE(lim->stats().gainDb);
    CHECK(*lim->stats().gainDb == Approx(-6.0).margin(0.1));

    // Тихий сигнал через какое-то время проходит без изменений.
    for (int f = 0; f < 200; ++f) {
        for (float& v : mic.channel(0)) v = 0.1f;
        lim->process(mic, nullptr);
    }
    CHECK(mic.channel(0)[fmt.frameSamples - 1] == Approx(0.1f).margin(1e-3));
}

TEST_CASE("Transient stage gates a sharp click, holds through the ring-down and passes a speech-like onset") {
    const PipelineFormat fmt;
    auto stage = makeTransientStage();
    const toml::table cfg;
    StageParams params(cfg);
    std::string error;
    REQUIRE(stage->init(fmt, params, error));
    const uint32_t n = fmt.frameSamples;
    Frame mic(1, n);
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    auto rms = [](std::span<const float> x) {
        double e = 0.0;
        for (const float v : x) e += double(v) * v;
        return std::sqrt(e / double(x.size()));
    };

    // Тихий шум комнаты: гейт молчит.
    for (int f = 0; f < 5; ++f) {
        for (float& v : mic.channel(0)) v = 0.001f * noise(rng);
        stage->process(mic, nullptr);
    }
    CHECK(stage->stats().transients == 0);

    // Щелчок -10 dBFS с середины кадра, затухание 3 ms: срабатывание, удар давится на глубину гейта.
    std::vector<float> in(n);
    for (uint32_t i = 0; i < n; ++i) {
        const float t = i >= 200 ? float(i - 200) / 48.0f : -1.0f;
        in[i] = 0.001f * noise(rng) + (t >= 0.0f ? 0.3f * std::exp(-t / 3.0f) * noise(rng) : 0.0f);
    }
    std::copy(in.begin(), in.end(), mic.channel(0).begin());
    stage->process(mic, nullptr);
    CHECK(stage->stats().transients == 1);
    const double clickIn = rms(std::span<const float>(in).subspan(200, 200));
    const double clickOut = rms(mic.channel(0).subspan(200, 200));
    CHECK(20.0 * std::log10(clickOut / clickIn) < -17.0);  // depth_db 20 минус запас
    // Пре-ролл: два блока перед щелчком тоже под гейтом, кадр до него нет.
    CHECK(mic.channel(0)[150] == Catch::Approx(in[150] * 0.1f).margin(1e-6f));
    CHECK(mic.channel(0)[50] == Catch::Approx(in[50]).margin(1e-7f));

    // Удержание 60 ms: следующие кадры ещё под гейтом; release 100 ms: через 400 ms
    // усиление 1 - 0.9 * exp(-3.4) = 0.97.
    for (int f = 0; f < 3; ++f) {
        for (float& v : mic.channel(0)) v = 0.05f * noise(rng);
        stage->process(mic, nullptr);
    }
    CHECK(rms(mic.channel(0)) < 0.05 * 0.2);
    for (int f = 0; f < 40; ++f) {
        for (float& v : mic.channel(0)) v = 0.05f * noise(rng);
        stage->process(mic, nullptr);
    }
    CHECK(rms(mic.channel(0)) > 0.05 * 0.95);
    CHECK(stage->stats().transients == 1);

    // Речевое начало: синус 200 Hz нарастает из тишины до -10 dBFS за 40 ms, не быстрее
    // 2 dB/ms там, где уровень уже выше level_dbfs. Гейт не срабатывает.
    stage->reset();
    uint32_t k = 0;
    for (int f = 0; f < 6; ++f) {
        for (float& v : mic.channel(0)) {
            const float a = std::min(1.0f, float(k) / (0.04f * 48000.0f));
            v = 0.3f * a * std::sin(2.0f * 3.14159265f * 200.0f * float(k) / 48000.0f);
            ++k;
        }
        stage->process(mic, nullptr);
    }
    CHECK(stage->stats().transients == 0);
    CHECK(rms(mic.channel(0)) == Catch::Approx(0.3 / std::sqrt(2.0)).epsilon(0.02));
}

TEST_CASE("Transient stage with the harmonicity check passes a voiced attack and still gates a click") {
    const PipelineFormat fmt;
    const uint32_t n = fmt.frameSamples;
    std::mt19937 rng(11);
    std::normal_distribution<float> noise(0.0f, 1.0f);

    auto make = [&](double harmonicity) {
        auto stage = makeTransientStage();
        toml::table cfg;
        cfg.insert("harmonicity_max", harmonicity);
        cfg.insert("lookahead_ms", 4.0);
        std::string error;
        StageParams params(cfg);
        REQUIRE(stage->init(fmt, params, error));
        return stage;
    };

    // Громкий согласный внутри голоса: тон 150 Hz на -11 dBFS с провалом 2 ms до -43 dBFS.
    // Выход из провала - та же скорость атаки, что у удара, но окно вокруг него гармонично:
    // на корпусе речи гейт срабатывал именно на таких местах.
    auto voiced = [&](IStage& stage) {
        uint32_t k = 0;
        uint64_t before = 0;
        for (int f = 0; f < 20; ++f) {
            Frame mic(1, n);
            for (uint32_t i = 0; i < n; ++i, ++k) {
                const bool dip = k >= 10 * n && k < 10 * n + 96;
                mic.channel(0)[i] =
                    float((dip ? 0.01 : 0.4) * std::sin(2.0 * std::numbers::pi * 150.0 * double(k) / fmt.sampleRate));
            }
            stage.process(mic, nullptr);
            if (f == 8) before = stage.stats().transients;  // старт тона из тишины не в счёт
        }
        return stage.stats().transients - before;
    };
    CHECK(voiced(*make(1.0)) > 0);  // без проверки гейт срабатывает на голосе
    CHECK(voiced(*make(0.4)) == 0);

    // Щелчок: шумовой всплеск -10 dBFS с затуханием 3 ms - гейт срабатывает и с проверкой.
    auto stage = make(0.4);
    uint64_t clicks = 0;
    double loudest = 0.0;
    for (int f = 0; f < 10; ++f) {
        Frame mic(1, n);
        for (uint32_t i = 0; i < n; ++i) {
            const float t = (f == 5 && i >= 200) ? float(i - 200) / 48.0f : -1.0f;
            mic.channel(0)[i] = 0.001f * noise(rng) + (t >= 0.0f ? 0.3f * std::exp(-t / 3.0f) * noise(rng) : 0.0f);
        }
        stage->process(mic, nullptr);
        clicks = stage->stats().transients;
        if (f >= 5) loudest = std::max(loudest, rms(mic.channel(0)));
    }
    CHECK(clicks == 1);
    CHECK(20.0 * std::log10(loudest / 0.001) < 25.0);  // без гейта всплеск был бы около 40 dB
}

TEST_CASE("Transient stage duck mode pulls a knock down to the voice under it and lets the voice back at once") {
    const PipelineFormat fmt;
    const uint32_t n = fmt.frameSamples;
    // Голос: тон 150 Hz на -37 dBFS; удар: шумовой всплеск -10 dBFS с затуханием 3 ms в кадре 10
    // (над голосом больше rise_db_per_ms, иначе детектор удар поверх голоса не видит).
    std::mt19937 rng(13);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    std::vector<float> in(20 * n);
    for (size_t k = 0; k < in.size(); ++k) {
        const double t = double(k) - double(10 * n + 200);
        const float knock = t >= 0.0 ? float(0.3 * std::exp(-t / 144.0)) * noise(rng) : 0.0f;
        in[k] = float(0.02 * std::sin(2.0 * std::numbers::pi * 150.0 * double(k) / fmt.sampleRate)) + knock;
    }
    auto run = [&](const char* mode, uint64_t& triggers) {
        auto stage = makeTransientStage();
        toml::table cfg;
        cfg.insert("mode", mode);
        cfg.insert("harmonicity_max", 0.4);
        cfg.insert("lookahead_ms", 4.0);
        cfg.insert("release_ms", 20.0);
        std::string error;
        StageParams params(cfg);
        REQUIRE(stage->init(fmt, params, error));
        std::vector<float> out;
        uint64_t before = 0;
        for (size_t f = 0; f < 20; ++f) {
            Frame mic(1, n);
            std::copy_n(in.begin() + std::ptrdiff_t(f * n), n, mic.channel(0).begin());
            stage->process(mic, nullptr);
            if (f == 5) before = stage->stats().transients;  // старт тона из тишины не в счёт
            out.insert(out.end(), mic.channel(0).begin(), mic.channel(0).end());
        }
        triggers = stage->stats().transients - before;
        return out;
    };
    const auto rmsDb = [](const std::vector<float>& x, size_t from, size_t count) {
        double e = 0.0;
        for (size_t i = from; i < from + count; ++i) e += double(x[i]) * x[i];
        return 10.0 * std::log10(e / double(count) + 1e-12);
    };
    uint64_t holdTriggers = 0, duckTriggers = 0;
    const std::vector<float> hold = run("hold", holdTriggers);
    const std::vector<float> duck = run("duck", duckTriggers);
    CHECK(holdTriggers == 1);
    CHECK(duckTriggers == 1);
    const size_t look = 4 * 48;  // выход отстаёт на lookahead
    const size_t knock = 10 * n + 200 + look;
    // Удар (первые 5 ms): оба режима давят; duck - до уровня голоса плюс margin, не на всю глубину.
    const double knockIn = rmsDb(in, 10 * n + 200, 240);
    const double knockHold = rmsDb(hold, knock, 240), knockDuck = rmsDb(duck, knock, 240);
    INFO("knock: in " << knockIn << ", hold " << knockHold << ", duck " << knockDuck << " dBFS");
    CHECK(knockHold < knockIn - 17.0);
    CHECK(knockDuck < knockIn - 8.0);
    // Голос через 30-50 ms после удара: hold ещё держит -20 dB, duck уже отпустил.
    const double voiceIn = rmsDb(in, 10 * n + 200 + 1440, 960);
    const double voiceHold = rmsDb(hold, knock + 1440, 960), voiceDuck = rmsDb(duck, knock + 1440, 960);
    INFO("voice after the knock: in " << voiceIn << ", hold " << voiceHold << ", duck " << voiceDuck << " dBFS");
    CHECK(voiceHold < voiceIn - 10.0);
    CHECK(voiceDuck > voiceIn - 2.0);
}

TEST_CASE("Transient stage keys are checked") {
    const PipelineFormat fmt;
    auto stage = makeTransientStage();
    toml::table cfg;
    cfg.insert("harmonicity_max", 1.5);  // вне 0..1
    std::string error;
    StageParams params(cfg);
    CHECK_FALSE(stage->init(fmt, params, error));

    toml::table modeCfg;
    modeCfg.insert("mode", "soft");  // только hold или duck
    StageParams modeParams(modeCfg);
    CHECK_FALSE(makeTransientStage()->init(fmt, modeParams, error));
}

TEST_CASE("RNNoise input_gain_db changes the operating point but not the output level") {
    const PipelineFormat fmt;
    auto level = [&](double gainDb) {
        auto stage = makeRnnoiseStage();
        toml::table cfg;
        cfg.insert("input_gain_db", gainDb);
        std::string error;
        StageParams params(cfg);
        REQUIRE(stage->init(fmt, params, error));
        return toneResponse(*stage, fmt, 500.0);
    };
    const double plain = level(0.0);
    const double loud = level(6.0);
    REQUIRE(plain > 0.0);
    // Без обратного деления после сети выход был бы на 6 dB громче.
    CHECK(std::fabs(20.0 * std::log10(loud / plain)) < 2.0);

    auto stage = makeRnnoiseStage();
    toml::table bad;
    bad.insert("input_gain_db", 100.0);
    std::string error;
    StageParams params(bad);
    CHECK_FALSE(stage->init(fmt, params, error));
}

TEST_CASE("WebRTC stage takes the AEC3 suppressor keys and rejects out-of-range ones") {
    const PipelineFormat fmt;
    auto stage = makeWebrtcStage();
    toml::table cfg;
    cfg.insert("nearend_mask_lf_transparent", 5.0);
    cfg.insert("nearend_mask_lf_suppress", 5.1);
    cfg.insert("nearend_enr_threshold", 0.35);
    cfg.insert("nearend_trigger_threshold", 3);
    cfg.insert("nearend_hold_duration", 100);
    cfg.insert("echo_path_default_gain", 0.4);
    std::string error;
    StageParams params(cfg);
    REQUIRE(stage->init(fmt, params, error));
    CHECK(params.unknownKeys().empty());

    toml::table bad;
    bad.insert("nearend_enr_threshold", -1.0);
    StageParams badParams(bad);
    CHECK_FALSE(stage->init(fmt, badParams, error));

    toml::table badGain;
    badGain.insert("echo_path_default_gain", 1.5);
    StageParams badGainParams(badGain);
    CHECK_FALSE(stage->init(fmt, badGainParams, error));
}

TEST_CASE("WebRTC stage cancels a delayed synthetic echo") {
    const PipelineFormat fmt;
    auto stage = makeWebrtcStage();
    toml::table cfg;
    cfg.insert("aec", true);
    cfg.insert("hpf", true);
    std::string error;
    StageParams params(cfg);
    REQUIRE(stage->init(fmt, params, error));
    CHECK(stage->info().caps == (capBit(Cap::Aec) | capBit(Cap::Hpf)));

    constexpr int kEchoDelay = 2400;  // 50 ms
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    std::vector<float> history(kEchoDelay, 0.0f);
    size_t hist = 0;

    Frame ref(2, fmt.frameSamples);
    Frame mic(1, fmt.frameSamples);
    double inEnergy = 0.0, outEnergy = 0.0;
    const int total = 500;
    for (int f = 0; f < total; ++f) {
        for (uint32_t i = 0; i < fmt.frameSamples; ++i) {
            const float s = noise(rng);
            ref.channel(0)[i] = s;
            ref.channel(1)[i] = 0.8f * s;
            const float echo = 0.5f * history[hist];
            history[hist] = 0.5f * (ref.channel(0)[i] + ref.channel(1)[i]);
            hist = (hist + 1) % kEchoDelay;
            mic.channel(0)[i] = echo;
        }
        if (f >= total - 100) inEnergy += rms(mic.channel(0));
        stage->process(mic, &ref);
        if (f >= total - 100) outEnergy += rms(mic.channel(0));
    }
    const double attenuationDb = 20.0 * std::log10(inEnergy / (outEnergy + 1e-9));
    INFO("attenuation dB = " << attenuationDb);
    CHECK(attenuationDb > 15.0);
    CHECK(stage->stats().erlDb.has_value());
}

TEST_CASE("WebRTC stage keeps near-end voice under an unrelated quiet reference with echo_path_default_gain") {
    // Слоги голоса в микрофоне и тихий низкочастотный шум в колонках, которого микрофон не
    // слышит. Пока фильтр AEC3 не сошёлся, он считает эхо равным reference с усилением
    // echo_path_default_gain: при штатном 1.0 голос того же уровня давится.
    const PipelineFormat fmt;
    constexpr int kFrames = 400;
    const size_t n = size_t(kFrames) * fmt.frameSamples;
    std::vector<float> voice(n), refL(n), refR(n);
    std::vector<bool> on(kFrames);
    double voiceE = 0.0, refE = 0.0;
    std::mt19937 rng(3);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    const float a = 1.0f - float(std::exp(-2.0 * std::numbers::pi * 300.0 / fmt.sampleRate));
    float l1 = 0, l2 = 0, r1 = 0, r2 = 0;  // два однополюсных ФНЧ 300 Hz на канал
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) / fmt.sampleRate;
        const bool syllable = std::fmod(t, 0.3) < 0.2;  // 200 ms голос, 100 ms пауза
        on[i / fmt.frameSamples] = syllable;
        double v = 0.0;
        for (int k = 1; k <= 11; ++k) v += std::sin(2.0 * std::numbers::pi * 150.0 * k * t) / k;
        voice[i] = syllable ? float(v) : 0.0f;
        voiceE += syllable ? v * v : 0.0;
        l1 += a * (noise(rng) - l1);
        l2 += a * (l1 - l2);
        r1 += a * (noise(rng) - r1);
        r2 += a * (r1 - r2);
        refL[i] = l2;
        refR[i] = r2;
        refE += double(l2) * l2;
    }
    const double onShare = double(std::count(on.begin(), on.end(), true)) / kFrames;
    const float voiceGain = float(std::pow(10.0, -30.0 / 20.0) / std::sqrt(voiceE / (double(n) * onShare)));
    const float refGain = float(std::pow(10.0, -40.0 / 20.0) / std::sqrt(refE / double(n)));

    // Уровни кадров выхода, dBFS.
    auto run = [&](double gain, bool withRef) {
        auto stage = makeWebrtcStage();
        toml::table cfg;
        cfg.insert("echo_path_default_gain", gain);
        std::string error;
        StageParams params(cfg);
        REQUIRE(stage->init(fmt, params, error));
        Frame mic(1, fmt.frameSamples), ref(2, fmt.frameSamples);
        std::vector<double> levels(kFrames);
        for (int f = 0; f < kFrames; ++f) {
            for (uint32_t i = 0; i < fmt.frameSamples; ++i) {
                const size_t k = size_t(f) * fmt.frameSamples + i;
                mic.channel(0)[i] = voiceGain * voice[k];
                ref.channel(0)[i] = withRef ? refGain * refL[k] : 0.0f;
                ref.channel(1)[i] = withRef ? refGain * refR[k] : 0.0f;
            }
            stage->process(mic, &ref);
            levels[size_t(f)] = 20.0 * std::log10(rms(mic.channel(0)) + 1e-9);
        }
        return levels;
    };
    // Доля кадров голоса (выход без reference громче -45 dBFS), просевших с reference > 6 dB.
    auto dippedShare = [&](double gain) {
        const std::vector<double> quiet = run(gain, false), loud = run(gain, true);
        size_t voiced = 0, dipped = 0;
        for (size_t f = 50; f < quiet.size(); ++f) {
            if (quiet[f] < -45.0) continue;
            ++voiced;
            if (loud[f] < quiet[f] - 6.0) ++dipped;
        }
        REQUIRE(voiced > 100);
        return double(dipped) / double(voiced);
    };
    const double stock = dippedShare(1.0), low = dippedShare(0.4);
    INFO("dipped share: gain 1.0 " << stock << ", gain 0.4 " << low);
    CHECK(stock > 0.4);
    CHECK(low < 0.2);
}

TEST_CASE("WebRTC stage without AEC still needs a feature, works without reference") {
    const PipelineFormat fmt;
    auto stage = makeWebrtcStage();
    toml::table none;
    none.insert("aec", false);
    std::string error;
    StageParams noneParams(none);
    CHECK_FALSE(stage->init(fmt, noneParams, error));

    toml::table nsOnly;
    nsOnly.insert("aec", false);
    nsOnly.insert("ns", true);
    nsOnly.insert("ns_level", "high");
    StageParams nsParams(nsOnly);
    REQUIRE(stage->init(fmt, nsParams, error));
    CHECK(stage->info().caps == capBit(Cap::Ns));
    Frame mic(1, fmt.frameSamples);
    stage->process(mic, nullptr);  // не должно падать без reference
}

TEST_CASE("Registry builds the default chain from config") {
    StageRegistry reg;
    registerBuiltinStages(reg);
    CHECK(reg.has("webrtc"));

    const char* text = R"(
[[chain]]
id = "hpf"
[[chain]]
id = "webrtc"
aec = true
[[chain]]
id = "limiter"
)";
    AppConfig cfg;
    std::string error;
    REQUIRE(parseConfig(text, cfg, error));
    auto chain = buildChain(reg, cfg, error);
    REQUIRE(chain);
    REQUIRE(chain->init(cfg.format, error));
    CHECK(chain->caps() == (capBit(Cap::Hpf) | capBit(Cap::Aec) | capBit(Cap::Limiter)));

    // Дубликат hpf (стадия hpf + webrtc с hpf=true) должен быть отклонён.
    cfg.chain[1].params.insert_or_assign("hpf", true);
    auto dup = buildChain(reg, cfg, error);
    REQUIRE(dup);
    CHECK_FALSE(dup->init(cfg.format, error));
}
