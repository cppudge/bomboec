#include "core/chain.h"
#include "stages/builtin_stages.h"

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("RNNoise stage attenuates stationary noise, keeps a voiced tone, delays by one frame", "[stages][rnnoise]") {
    const PipelineFormat fmt;
    auto stage = makeRnnoiseStage();
    const toml::table cfg;
    StageParams params(cfg);
    std::string error;
    REQUIRE(stage->init(fmt, params, error));
    CHECK(stage->info().caps == capBit(Cap::Ns));
    CHECK(stage->info().latencyFrames == 1);

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

    // Задержка: одиночный щелчок выходит в следующем кадре (перекрытие окон анализа).
    stage->reset();
    for (int f = 0; f < 20; ++f) {
        for (float& x : mic.channel(0)) x = 0.0f;
        stage->process(mic, nullptr);
    }
    for (float& x : mic.channel(0)) x = 0.0f;
    mic.channel(0)[100] = 0.5f;
    stage->process(mic, nullptr);
    const double sameFrame = rms(mic.channel(0));
    for (float& x : mic.channel(0)) x = 0.0f;
    stage->process(mic, nullptr);
    const double nextFrame = rms(mic.channel(0));
    INFO("click energy: same frame " << sameFrame << ", next frame " << nextFrame);
    CHECK(nextFrame > sameFrame * 3.0);
}

TEST_CASE("RNNoise stage rejects other formats and duplicates the ns capability of webrtc", "[stages][rnnoise]") {
    auto stage = makeRnnoiseStage();
    PipelineFormat fmt;
    fmt.sampleRate = 44100;
    fmt.frameSamples = 441;
    const toml::table cfg;
    StageParams params(cfg);
    std::string error;
    CHECK_FALSE(stage->init(fmt, params, error));

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
}
