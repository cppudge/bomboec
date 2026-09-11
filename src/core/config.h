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

// Устройства и параметры realtime-движка ([devices], [engine]).
struct EngineSettings {
    std::string micId;        // endpoint ID; пусто = default capture
    std::string speakersId;   // render endpoint для loopback; пусто = default render
    std::string outputId;     // render endpoint для очищенного сигнала (virtual cable)
    bool micRaw = true;
    uint32_t referenceLeadMs = 20;   // reference берётся на столько раньше mic (запас на джиттер loopback)
    uint32_t outputBufferMs = 30;    // предзаполнение выходного буфера
    uint32_t outputChannels = 2;
    std::string recordDir;           // непусто: debug-запись mic_raw/ref/out в WAV
};

struct AppConfig {
    PipelineFormat format;
    std::vector<StageConfig> chain;
    EngineSettings engine;
    toml::table raw;  // исходный документ для сохранения с правками
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

// Записывает cfg.raw с актуальными [devices]/[engine] из cfg.engine.
bool saveConfig(const std::filesystem::path& path, const AppConfig& cfg, std::string& error);

}  // namespace bomboec
