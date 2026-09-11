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
    std::string micId;       // endpoint ID; пусто = default capture
    std::string speakersId;  // render endpoint для loopback; пусто = default render
    std::string outputId;    // render endpoint для очищенного сигнала (virtual cable)
    // Имена устройств: запасной способ найти устройство, если endpoint ID
    // сменился (переустановка драйвера кабеля меняет ID, имя остаётся).
    std::string micName, speakersName, outputName;
    bool micRaw = true;
    uint32_t referenceLeadMs = 20;  // reference берётся на столько раньше mic (запас на джиттер loopback)
    uint32_t outputBufferMs = 10;   // целевой запас в выходном кольце после чтения render-потоком
    uint32_t outputRenderMs = 20;   // целевое заполнение буфера WASAPI выхода (>= 2 периодов engine)
    uint32_t outputChannels = 2;
    std::string recordDir;  // непусто: debug-запись mic_raw/ref/out в WAV
};

struct AppConfig {
    PipelineFormat format;
    std::vector<StageConfig> chain;
    EngineSettings engine;
    // Конфиг читается, но что-то в нём, скорее всего, не так (опечатка в ключе).
    std::vector<std::string> warnings;
};

// Формат файла: config/default.toml. Проверяются типы и диапазоны ключей [format],
// [devices] и [engine]; ошибка значения - отказ с текстом в error. Неизвестные ключи
// (опечатка не должна молча оставлять значение по умолчанию) идут в warnings. Ключи
// стадий [[chain]] проверяют сами стадии в init().
bool parseConfig(std::string_view text, AppConfig& out, std::string& error);
bool loadConfig(const std::filesystem::path& path, AppConfig& out, std::string& error);

// Состояние приложения (устройства, выбранные в меню трея) живёт в отдельном файле
// (bomboec.state.toml): сохранение не трогает комментарии и правки пользователя в
// конфиге. Если файл есть, его [devices] перекрывает [devices] конфига целиком.
// loadState без файла возвращает true и ничего не меняет.
bool loadState(const std::filesystem::path& path, EngineSettings& settings, std::string& error);
bool saveState(const std::filesystem::path& path, const EngineSettings& settings, std::string& error);

// Запись через временный файл рядом и замену: сбой посередине не оставляет пустой
// или обрезанный файл (пустой конфиг читался бы как цепочка без AEC).
bool writeFileAtomic(const std::filesystem::path& path, std::string_view text, std::string& error);

// Шаблон конфигурации по умолчанию: config/default.toml, встроенный при сборке.
std::string_view defaultConfigToml();

}  // namespace bomboec
