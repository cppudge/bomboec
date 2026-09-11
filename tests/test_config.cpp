#include "core/config.h"

#include <catch2/catch_test_macros.hpp>

using namespace bomboec;

TEST_CASE("Config parses format and chain") {
    const char* text = R"(
[format]
sample_rate = 48000
frame_ms = 10
reference_channels = 2

[[chain]]
id = "webrtc"
aec = true
filter_length_blocks = 20

[[chain]]
id = "limiter"
ceiling_db = -1.0
)";
    AppConfig cfg;
    std::string error;
    REQUIRE(parseConfig(text, cfg, error));
    CHECK(cfg.format.sampleRate == 48000);
    CHECK(cfg.format.frameSamples == 480);
    CHECK(cfg.format.micChannels == 1);
    CHECK(cfg.format.referenceChannels == 2);
    REQUIRE(cfg.chain.size() == 2);
    CHECK(cfg.chain[0].id == "webrtc");
    CHECK(cfg.chain[0].params["filter_length_blocks"].value_or(0) == 20);
    CHECK_FALSE(cfg.chain[0].params.contains("id"));
    CHECK(cfg.chain[1].params["ceiling_db"].value_or(0.0) == -1.0);
}

TEST_CASE("Config defaults and errors") {
    AppConfig cfg;
    std::string error;
    REQUIRE(parseConfig("", cfg, error));
    CHECK(cfg.format.frameSamples == 480);
    CHECK(cfg.chain.empty());

    CHECK_FALSE(parseConfig("[[chain]]\naec = true\n", cfg, error));
    CHECK(error.find("id") != std::string::npos);

    CHECK_FALSE(parseConfig("[format]\nsample_rate = 0\n", cfg, error));
    CHECK_FALSE(parseConfig("this is = not = toml", cfg, error));
}

TEST_CASE("Embedded default config parses and repeats the code defaults") {
    AppConfig cfg;
    std::string error;
    REQUIRE(parseConfig(defaultConfigToml(), cfg, error));
    CHECK(cfg.format.frameSamples == 480);
    REQUIRE(cfg.chain.size() == 2);
    CHECK(cfg.chain[0].id == "webrtc");
    CHECK(cfg.chain[1].id == "limiter");
    // Шаблон не должен менять поведение относительно пустого конфига.
    const EngineSettings defaults;
    CHECK(cfg.engine.micId.empty());
    CHECK(cfg.engine.outputId.empty());
    CHECK(cfg.engine.micRaw == defaults.micRaw);
    CHECK(cfg.engine.referenceLeadMs == defaults.referenceLeadMs);
    CHECK(cfg.engine.outputBufferMs == defaults.outputBufferMs);
    CHECK(cfg.engine.outputRenderMs == defaults.outputRenderMs);
    CHECK(cfg.engine.outputChannels == defaults.outputChannels);
}
