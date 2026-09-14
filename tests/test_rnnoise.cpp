#include "core/chain.h"
#include "stages/builtin_stages.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace bomboec;

namespace {

double rms(std::span<const float> x) {
    double e = 0.0;
    for (const float v : x) e += double(v) * v;
    return std::sqrt(e / double(x.size()));
}

}  // namespace

TEST_CASE("RNNoise stage attenuates stationary noise, keeps a voiced tone, delays by two frames", "[stages][rnnoise]") {
    const PipelineFormat fmt;
    auto stage = makeRnnoiseStage();
    const toml::table cfg;
    StageParams params(cfg);
    std::string error;
    REQUIRE(stage->init(fmt, params, error));
    CHECK(stage->info().caps == capBit(Cap::Ns));
    CHECK(stage->info().latencyFrames == 2);

    Frame mic(1, fmt.frameSamples);
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, 0.02f);  // -34 dBFS: шум вентилятора

    // Белый шум: после разгона сети подавление не меньше 10 dB.
    double in = 0.0, out = 0.0;
    for (int f = 0; f < 300; ++f) {
        for (float& x : mic.channel(0)) x = noise(rng);
        if (f >= 200) in += rms(mic.channel(0));
        stage->process(mic, nullptr);
        if (f >= 200) out += rms(mic.channel(0));
    }
    const double noiseAttenuationDb = 20.0 * std::log10(in / (out + 1e-9));
    INFO("noise attenuation " << noiseAttenuationDb << " dB");
    CHECK(noiseAttenuationDb > 10.0);
    REQUIRE(stage->stats().vadProbability);
    CHECK(*stage->stats().vadProbability < 0.5);

    // Гармонический сигнал с основным тоном 140 Hz и обертонами (похож на голос):
    // проходит с потерей не больше 3 dB.
    stage->reset();
    in = out = 0.0;
    for (int f = 0; f < 300; ++f) {
        for (uint32_t i = 0; i < fmt.frameSamples; ++i) {
            const double t = double(f * fmt.frameSamples + i) / fmt.sampleRate;
            double v = 0.0;
            for (int h = 1; h <= 12; ++h) v += std::sin(2.0 * std::numbers::pi * 140.0 * h * t) / h;
            mic.channel(0)[i] = float(0.1 * v * (0.6 + 0.4 * std::sin(2.0 * std::numbers::pi * 4.0 * t)));
        }
        if (f >= 200) in += rms(mic.channel(0));
        stage->process(mic, nullptr);
        if (f >= 200) out += rms(mic.channel(0));
    }
    const double toneLossDb = 20.0 * std::log10(in / (out + 1e-9));
    INFO("tone loss " << toneLossDb << " dB");
    CHECK(toneLossDb < 3.0);

    // Задержка ровно два кадра: окно анализа с перекрытием плюс усиление нового кадра на
    // предыдущем спектре. Щелчок на сэмпле 100 кадра 0 выходит на сэмпле 100 кадра 2.
    stage->reset();
    for (int f = 0; f < 20; ++f) {
        for (float& x : mic.channel(0)) x = 0.0f;
        stage->process(mic, nullptr);
    }
    std::vector<float> clickOut;
    for (int f = 0; f < 4; ++f) {
        for (float& x : mic.channel(0)) x = 0.0f;
        if (f == 0) mic.channel(0)[100] = 0.5f;
        stage->process(mic, nullptr);
        clickOut.insert(clickOut.end(), mic.channel(0).begin(), mic.channel(0).end());
    }
    const auto loudest = std::max_element(clickOut.begin(), clickOut.end(),
                                          [](float a, float b) { return std::fabs(a) < std::fabs(b); });
    const auto peak = size_t(loudest - clickOut.begin());
    INFO("click peak at output sample " << peak);
    CHECK(peak == 2 * fmt.frameSamples + 100);
}

TEST_CASE("RNNoise stage gate: a click is gated on the network output without extra delay, a voiced tone is not",
          "[stages][rnnoise]") {
    const PipelineFormat fmt;
    const uint32_t n = fmt.frameSamples;
    auto make = [&](bool gate) {
        auto stage = makeRnnoiseStage();
        toml::table cfg;
        cfg.insert("gate", gate);
        std::string error;
        StageParams params(cfg);
        REQUIRE(stage->init(fmt, params, error));
        CHECK(params.unknownKeys().empty());
        return stage;
    };
    auto on = make(true);
    auto off = make(false);
    CHECK(on->info().latencyFrames == 2);
    CHECK(on->info().caps == (capBit(Cap::Ns) | capBit(Cap::Transient)));
    CHECK(off->info().caps == capBit(Cap::Ns));

    // Щелчок -10 dBFS с затуханием 3 ms в кадре 30 поверх тихого шума: на выходе сети он в
    // кадре 32, и там гейт его давит; кадры до щелчка с гейтом и без него одинаковы.
    std::mt19937 rng(3);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    std::vector<std::vector<float>> input(40, std::vector<float>(n));
    for (size_t f = 0; f < input.size(); ++f) {
        for (uint32_t i = 0; i < n; ++i) {
            const float t = (f == 30 && i >= 200) ? float(i - 200) / 48.0f : -1.0f;
            input[f][i] = 0.003f * noise(rng) + (t >= 0.0f ? 0.3f * std::exp(-t / 3.0f) * noise(rng) : 0.0f);
        }
    }
    double clickOn = 0.0, clickOff = 0.0;
    bool sameBefore = true;
    for (size_t f = 0; f < input.size(); ++f) {
        Frame a(1, n), b(1, n);
        std::copy(input[f].begin(), input[f].end(), a.channel(0).begin());
        std::copy(input[f].begin(), input[f].end(), b.channel(0).begin());
        on->process(a, nullptr);
        off->process(b, nullptr);
        if (f < 32)
            sameBefore = sameBefore && std::equal(a.channel(0).begin(), a.channel(0).end(), b.channel(0).begin());
        if (f == 32) {
            clickOn = rms(a.channel(0));
            clickOff = rms(b.channel(0));
        }
    }
    INFO("click frame rms: gate " << clickOn << ", no gate " << clickOff);
    CHECK(sameBefore);
    CHECK(on->stats().transients == 1);
    CHECK(20.0 * std::log10(clickOn / clickOff) < -12.0);

    // Громкий тон с провалом 2 ms (атака как у удара, но окно гармонично): гейт молчит.
    auto voiced = make(true);
    uint32_t k = 0;
    uint64_t before = 0;
    for (int f = 0; f < 30; ++f) {
        Frame mic(1, n);
        for (uint32_t i = 0; i < n; ++i, ++k) {
            const bool dip = k >= 20 * n && k < 20 * n + 96;
            mic.channel(0)[i] =
                float((dip ? 0.01 : 0.4) * std::sin(2.0 * std::numbers::pi * 150.0 * double(k) / fmt.sampleRate));
        }
        voiced->process(mic, nullptr);
        if (f == 10) before = voiced->stats().transients;  // старт тона из тишины не в счёт
    }
    CHECK(voiced->stats().transients == before);
}

TEST_CASE("RNNoise stage rejects other formats, a second ns and a second transient gate", "[stages][rnnoise]") {
    auto stage = makeRnnoiseStage();
    PipelineFormat fmt;
    fmt.sampleRate = 44100;
    fmt.frameSamples = 441;
    const toml::table cfg;
    StageParams params(cfg);
    std::string error;
    CHECK_FALSE(stage->init(fmt, params, error));
    CHECK(error.find("48 kHz") != std::string::npos);

    // Ключ гейта вне диапазона: ошибка называет его один раз, с префиксом gate_.
    toml::table badGate;
    badGate.insert("gate_depth_db", 100.0);
    StageParams badParams(badGate);
    CHECK_FALSE(makeRnnoiseStage()->init(PipelineFormat{}, badParams, error));
    CHECK(error.rfind("rnnoise: gate_depth_db ", 0) == 0);

    StageRegistry reg;
    registerBuiltinStages(reg);
    const char* text = R"(
[[chain]]
id = "webrtc"
aec = true
ns = true
[[chain]]
id = "rnnoise"
)";
    AppConfig appCfg;
    REQUIRE(parseConfig(text, appCfg, error));
    auto chain = buildChain(reg, appCfg, error);
    REQUIRE(chain);
    CHECK_FALSE(chain->init(appCfg.format, error));
    CHECK(error.find("'ns'") != std::string::npos);

    // Гейт внутри rnnoise и отдельная стадия transient вместе давили бы удар дважды.
    const char* twoGates = R"(
[[chain]]
id = "transient"
[[chain]]
id = "rnnoise"
gate = true
)";
    AppConfig gatesCfg;
    REQUIRE(parseConfig(twoGates, gatesCfg, error));
    auto gates = buildChain(reg, gatesCfg, error);
    REQUIRE(gates);
    CHECK_FALSE(gates->init(gatesCfg.format, error));
    CHECK(error.find("'transient'") != std::string::npos);
}
