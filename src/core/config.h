#pragma once

#include "core/audio_format.h"

#include <toml++/toml.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace bomboec {

struct StageConfig {
    std::string id;
    toml::table params;  // таблица стадии без ключа id
};

struct AppConfig {
    PipelineFormat format;
    std::vector<StageConfig> chain;
};

// Формат файла:
//
//   [format]
//   sample_rate = 48000
//   frame_ms = 10
//   reference_channels = 2
//
//   [[chain]]
//   id = "webrtc"
//   aec = true
//
//   [[chain]]
//   id = "limiter"
//   ceiling_db = -1.0
bool parseConfig(std::string_view text, AppConfig& out, std::string& error);
bool loadConfig(const std::filesystem::path& path, AppConfig& out, std::string& error);

}  // namespace bomboec
