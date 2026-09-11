#include "core/chain.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace bomboec;

namespace {

// Тестовая стадия: умножает сигнал на gain, объявляет заданные caps.
class GainStage final : public IStage {
public:
    GainStage(std::string id, uint32_t caps, uint32_t latency, std::vector<std::string>* log)
        : id_(std::move(id)), caps_(caps), latency_(latency), log_(log) {}

    StageInfo info() const override { return {id_, fmt_.sampleRate, fmt_.frameSamples, caps_, latency_}; }

    bool init(const PipelineFormat& fmt, const toml::table& cfg, std::string& error) override {
        fmt_ = fmt;
        gain_ = float(cfg["gain"].value_or(1.0));
        if (cfg["fail"].value_or(false)) {
            error = "asked to fail";
            return false;
        }
        return true;
    }

    void process(Frame& mic, const Frame* ref) override {
        if (log_) log_->push_back(id_ + (ref ? "+ref" : ""));
        for (float& x : mic.channel(0)) x *= gain_;
    }

    void reset() override { resets++; }

    StageStats stats() const override {
        StageStats s;
        if (hasCap(caps_, Cap::Aec)) s.erleDb = 12.0;
        if (hasCap(caps_, Cap::Limiter)) s.gainDb = -3.0;
        return s;
    }

    int resets = 0;

private:
    std::string id_;
    uint32_t caps_;
    uint32_t latency_;
    std::vector<std::string>* log_;
    PipelineFormat fmt_;
    float gain_ = 1.0f;
};

}  // namespace

TEST_CASE("Chain runs stages in order and aggregates caps/latency/stats") {
    std::vector<std::string> log;
    Chain chain;
    toml::table p1;
    p1.insert("gain", 2.0);
    toml::table p2;
    p2.insert("gain", 3.0);
    chain.add(std::make_unique<GainStage>("a", capBit(Cap::Aec), 1, &log), p1);
    chain.add(std::make_unique<GainStage>("b", capBit(Cap::Limiter), 2, &log), p2);

    std::string error;
    const PipelineFormat fmt;
    REQUIRE(chain.init(fmt, error));
    CHECK(chain.caps() == (capBit(Cap::Aec) | capBit(Cap::Limiter)));
    CHECK(chain.latencyFrames() == 3);

    Frame mic(1, fmt.frameSamples);
    const Frame ref(2, fmt.frameSamples);
    mic.channel(0)[0] = 1.0f;
    chain.process(mic, &ref);
    CHECK(mic.channel(0)[0] == 6.0f);
    REQUIRE(log.size() == 2);
    CHECK(log[0] == "a+ref");
    CHECK(log[1] == "b+ref");

    const StageStats s = chain.stats();
    REQUIRE(s.erleDb);
    CHECK(*s.erleDb == 12.0);
    REQUIRE(s.gainDb);
    CHECK(*s.gainDb == -3.0);
    CHECK_FALSE(s.delayMs);

    chain.reset();
    CHECK(static_cast<GainStage&>(chain.stage(0)).resets == 1);
}

TEST_CASE("Chain rejects duplicate capabilities and failing init") {
    Chain chain;
    chain.add(std::make_unique<GainStage>("a", capBit(Cap::Ns), 0, nullptr));
    chain.add(std::make_unique<GainStage>("b", capBit(Cap::Ns) | capBit(Cap::Agc), 0, nullptr));
    std::string error;
    CHECK_FALSE(chain.init(PipelineFormat{}, error));
    CHECK(error.find("'ns'") != std::string::npos);

    Chain failing;
    toml::table p;
    p.insert("fail", true);
    failing.add(std::make_unique<GainStage>("x", 0, 0, nullptr), p);
    CHECK_FALSE(failing.init(PipelineFormat{}, error));
    CHECK(error.find("asked to fail") != std::string::npos);
}

TEST_CASE("buildChain uses registry ids") {
    StageRegistry reg;
    reg.add("gain", [] { return std::make_unique<GainStage>("gain", 0, 0, nullptr); });

    AppConfig cfg;
    cfg.chain.push_back({"gain", {}});
    std::string error;
    auto chain = buildChain(reg, cfg, error);
    REQUIRE(chain);
    CHECK(chain->size() == 1);

    cfg.chain.push_back({"nope", {}});
    CHECK_FALSE(buildChain(reg, cfg, error));
    CHECK(error.find("nope") != std::string::npos);
}
