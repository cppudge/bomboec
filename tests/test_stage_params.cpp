#include "core/chain.h"
#include "core/stage_params.h"
#include "stages/builtin_stages.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace bomboec;

TEST_CASE("StageParams reads typed values, rejects wrong types and ranges, lists unread keys", "[stage-params]") {
    toml::table t;
    t.insert("flag", true);
    t.insert("count", int64_t(20));
    t.insert("ratio", 0.5);
    t.insert("whole", int64_t(3));  // целое там, где ждут число
    t.insert("mode", "high");
    t.insert("typo_key", 1);

    SECTION("valid reads") {
        StageParams p(t);
        CHECK(p.boolean("flag", false) == true);
        CHECK(p.boolean("absent", true) == true);
        CHECK(p.integer("count", 1, 1, 60) == 20);
        CHECK(p.number("ratio", 1.0, 0.0, 1.0) == 0.5);
        CHECK(p.number("whole", 1.0, 0.0, 10.0) == 3.0);
        CHECK(p.string("mode", "x") == "high");
        CHECK(p.choice("mode", "low", {"low", "high"}) == "high");
        CHECK(p.ok());
        const std::vector<std::string> unknown = p.unknownKeys();
        REQUIRE(unknown.size() == 1);
        CHECK(unknown[0] == "typo_key");
    }
    SECTION("wrong type is an error, not the default") {
        StageParams p(t);
        CHECK(p.boolean("count", false) == false);
        CHECK_FALSE(p.ok());
        CHECK(p.error().find("count") != std::string::npos);
    }
    SECTION("out of range") {
        StageParams p(t);
        CHECK(p.integer("count", 13, 1, 10) == 13);
        CHECK_FALSE(p.ok());
        CHECK(p.error().find("1..10") != std::string::npos);
    }
    SECTION("choice outside the list") {
        StageParams p(t);
        CHECK(p.choice("mode", "low", {"low", "moderate"}) == "low");
        CHECK_FALSE(p.ok());
        CHECK(p.error().find("low|moderate") != std::string::npos);
    }
}

TEST_CASE("Stages reject mistyped keys and Chain reports typos", "[stage-params]") {
    StageRegistry reg;
    registerBuiltinStages(reg);
    std::string error;

    // Раньше value_or глотал и то, и другое: aec = "yes" давало true, опечатка - умолчание.
    const char* mistyped = R"(
[[chain]]
id = "webrtc"
aec = "yes"
)";
    AppConfig cfg;
    REQUIRE(parseConfig(mistyped, cfg, error));
    auto chain = buildChain(reg, cfg, error);
    REQUIRE(chain);
    CHECK_FALSE(chain->init(cfg.format, error));
    CHECK(error.find("aec") != std::string::npos);

    const char* typo = R"(
[[chain]]
id = "webrtc"
aec = true
filter_lenght_blocks = 20
[[chain]]
id = "limiter"
ceiling_db = -3
)";
    REQUIRE(parseConfig(typo, cfg, error));
    chain = buildChain(reg, cfg, error);
    REQUIRE(chain);
    REQUIRE(chain->init(cfg.format, error));
    REQUIRE(chain->warnings().size() == 1);
    CHECK(chain->warnings()[0].find("filter_lenght_blocks") != std::string::npos);
    CHECK(chain->warnings()[0].find("webrtc") != std::string::npos);

    const char* outOfRange = R"(
[[chain]]
id = "limiter"
release_ms = 0
)";
    REQUIRE(parseConfig(outOfRange, cfg, error));
    chain = buildChain(reg, cfg, error);
    REQUIRE(chain);
    CHECK_FALSE(chain->init(cfg.format, error));
    CHECK(error.find("release_ms") != std::string::npos);
}
