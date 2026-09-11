#include "core/chain.h"
#include "stages/builtin_stages.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    REQUIRE(hpf->init(fmt, cfg, error));
    CHECK(hpf->info().caps == capBit(Cap::Hpf));

    const double ref = 0.5 / std::numbers::sqrt2;
    const double low = toneResponse(*hpf, fmt, 20.0);
    hpf->reset();
    const double high = toneResponse(*hpf, fmt, 1000.0);
    CHECK(20.0 * std::log10(low / ref) < -20.0);
    CHECK(20.0 * std::log10(high / ref) == Approx(0.0).margin(0.2));

    toml::table bad;
    bad.insert("cutoff_hz", 30000.0);
    CHECK_FALSE(hpf->init(fmt, bad, error));
}

TEST_CASE("Limiter stage keeps peaks under the ceiling") {
    const PipelineFormat fmt;
    auto lim = makeLimiterStage();
    toml::table cfg;
    cfg.insert("ceiling_db", -6.0);
    std::string error;
    REQUIRE(lim->init(fmt, cfg, error));

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

TEST_CASE("WebRTC stage cancels a delayed synthetic echo") {
    const PipelineFormat fmt;
    auto stage = makeWebrtcStage();
    toml::table cfg;
    cfg.insert("aec", true);
    cfg.insert("hpf", true);
    std::string error;
    REQUIRE(stage->init(fmt, cfg, error));
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

TEST_CASE("WebRTC stage without AEC still needs a feature, works without reference") {
    const PipelineFormat fmt;
    auto stage = makeWebrtcStage();
    toml::table none;
    none.insert("aec", false);
    std::string error;
    CHECK_FALSE(stage->init(fmt, none, error));

    toml::table nsOnly;
    nsOnly.insert("aec", false);
    nsOnly.insert("ns", true);
    nsOnly.insert("ns_level", "high");
    REQUIRE(stage->init(fmt, nsOnly, error));
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
