#include "core/config.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace bomboec;

namespace {

bool mentions(const std::vector<std::string>& lines, const std::string& text) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(text) != l.npos; });
}

}  // namespace

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
    CHECK(cfg.warnings.empty());
    CHECK(cfg.format.frameSamples == 480);
    REQUIRE(cfg.chain.size() == 3);
    CHECK(cfg.chain[0].id == "webrtc");
    CHECK(cfg.chain[1].id == "rnnoise");
    CHECK(cfg.chain[2].id == "limiter");
    // Шаблон не должен менять поведение относительно пустого конфига.
    const EngineSettings defaults;
    CHECK(cfg.engine.micId.empty());
    CHECK(cfg.engine.outputId.empty());
    CHECK(cfg.engine.micRaw == defaults.micRaw);
    CHECK(cfg.engine.referenceLeadMs == defaults.referenceLeadMs);
    CHECK(cfg.engine.outputBufferMs == defaults.outputBufferMs);
    CHECK(cfg.engine.outputRenderMs == defaults.outputRenderMs);
    CHECK(cfg.engine.outputChannels == defaults.outputChannels);
    CHECK(cfg.engine.onDemand == defaults.onDemand);
    CHECK(cfg.engine.idleStopSec == defaults.idleStopSec);
}

TEST_CASE("Config rejects out-of-range and mistyped values") {
    AppConfig cfg;
    std::string error;
    // Раньше -1 превращался в 4294967295 и ронял движок огромной аллокацией.
    CHECK_FALSE(parseConfig("[format]\nsample_rate = -1\n", cfg, error));
    CHECK(error.find("format.sample_rate") != std::string::npos);
    CHECK_FALSE(parseConfig("[format]\nmic_channels = 100\n", cfg, error));
    CHECK_FALSE(parseConfig("[format]\nframe_ms = 0.1\n", cfg, error));
    CHECK_FALSE(parseConfig("[engine]\noutput_buffer_ms = \"10\"\n", cfg, error));
    CHECK_FALSE(parseConfig("[engine]\nmic_raw = 1\n", cfg, error));
    CHECK_FALSE(parseConfig("[devices]\nmic = 5\n", cfg, error));
    CHECK_FALSE(parseConfig("format = 5\n", cfg, error));
    CHECK_FALSE(parseConfig("[chain]\nid = \"webrtc\"\n", cfg, error));  // таблица вместо [[chain]]
    CHECK(error.find("[[chain]]") != std::string::npos);

    REQUIRE(parseConfig("[format]\nframe_ms = 10\n[engine]\nreference_lead_ms = 0\n", cfg, error));
    CHECK(cfg.format.frameSamples == 480);  // целое для дробного ключа допустимо
    CHECK(cfg.engine.referenceLeadMs == 0);
}

TEST_CASE("Config reports unknown keys as warnings") {
    AppConfig cfg;
    std::string error;
    REQUIRE(parseConfig("[engine]\noutput_bufer_ms = 5\n[fromat]\nsample_rate = 44100\n", cfg, error));
    CHECK(cfg.warnings.size() == 2);
    CHECK(mentions(cfg.warnings, "engine.output_bufer_ms"));
    CHECK(mentions(cfg.warnings, "fromat"));
    CHECK(cfg.engine.outputBufferMs == EngineSettings{}.outputBufferMs);
}

TEST_CASE("State file overrides the devices and is replaced atomically") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("bomboec-config-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    const std::filesystem::path state = dir / "bomboec.state.toml";
    std::string error;

    EngineSettings s;
    CHECK(loadState(state, s, error));  // файла нет: ничего не меняется
    CHECK(s.micId.empty());

    s.micId = "{mic}";
    s.micName = "Микрофон (Yeti Orb)";
    s.outputId = "{out}";
    REQUIRE(saveState(state, s, error));
    REQUIRE(saveState(state, s, error));  // замена существующего файла
    CHECK_FALSE(std::filesystem::exists(dir / "bomboec.state.toml.tmp"));

    AppConfig cfg;
    REQUIRE(parseConfig("[devices]\nmic = \"{config-mic}\"\nspeakers = \"{config-speakers}\"\n", cfg, error));
    REQUIRE(loadState(state, cfg.engine, error));
    CHECK(cfg.engine.micId == "{mic}");
    CHECK(cfg.engine.micName == "Микрофон (Yeti Orb)");
    CHECK(cfg.engine.outputId == "{out}");
    CHECK(cfg.engine.speakersId.empty());  // выбор "System default" в меню тоже перекрывает конфиг
    CHECK(cfg.engine.micRaw == EngineSettings{}.micRaw);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
