#include "core/config.h"

#include "core/utf8.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <sstream>

namespace bomboec {

namespace {

std::string keyName(std::string_view section, std::string_view key) {
    return section.empty() ? std::string(key) : std::string(section) + "." + std::string(key);
}

void checkKeys(const toml::table& t, std::string_view section, std::initializer_list<std::string_view> known,
               std::vector<std::string>& warnings) {
    for (const auto& entry : t) {
        const std::string_view key = entry.first.str();
        if (std::find(known.begin(), known.end(), key) == known.end()) {
            warnings.push_back("config: unknown key '" + keyName(section, key) + "' ignored");
        }
    }
}

// Таблица name или nullptr, если её нет; не таблица - ошибка.
bool tableAt(const toml::table& root, std::string_view name, const toml::table*& table, std::string& error) {
    const toml::node* n = root.get(name);
    table = n ? n->as_table() : nullptr;
    if (n && !table) {
        error = "config: '" + std::string(name) + "' must be a table ([" + std::string(name) + "])";
        return false;
    }
    return true;
}

// Читатели ключей: ключа нет - value не меняется; не тот тип или вне диапазона - ошибка.
bool readUint(const toml::table& t, std::string_view section, std::string_view key, int64_t lo, int64_t hi,
              uint32_t& value, std::string& error) {
    const toml::node* n = t.get(key);
    if (!n) return true;
    const std::optional<int64_t> v = n->value_exact<int64_t>();
    if (!v || *v < lo || *v > hi) {
        error = "config: " + keyName(section, key) + " must be an integer in " + std::to_string(lo) + ".." +
                std::to_string(hi);
        return false;
    }
    value = uint32_t(*v);
    return true;
}

bool readNumber(const toml::table& t, std::string_view section, std::string_view key, double lo, double hi,
                double& value, std::string& error) {
    const toml::node* n = t.get(key);
    if (!n) return true;
    const std::optional<double> v = n->value<double>();
    if (!v || !(*v >= lo && *v <= hi)) {
        std::ostringstream oss;
        oss << "config: " << keyName(section, key) << " must be a number in " << lo << ".." << hi;
        error = oss.str();
        return false;
    }
    value = *v;
    return true;
}

bool readBool(const toml::table& t, std::string_view section, std::string_view key, bool& value, std::string& error) {
    const toml::node* n = t.get(key);
    if (!n) return true;
    const std::optional<bool> v = n->value_exact<bool>();
    if (!v) {
        error = "config: " + keyName(section, key) + " must be true or false";
        return false;
    }
    value = *v;
    return true;
}

bool readString(const toml::table& t, std::string_view section, std::string_view key, std::string& value,
                std::string& error) {
    const toml::node* n = t.get(key);
    if (!n) return true;
    std::optional<std::string> v = n->value_exact<std::string>();
    if (!v) {
        error = "config: " + keyName(section, key) + " must be a string";
        return false;
    }
    value = std::move(*v);
    return true;
}

bool readDevices(const toml::table& dev, EngineSettings& s, std::vector<std::string>* warnings, std::string& error) {
    if (warnings) {
        checkKeys(dev, "devices", {"mic", "speakers", "output", "mic_name", "speakers_name", "output_name"}, *warnings);
    }
    return readString(dev, "devices", "mic", s.micId, error) &&
           readString(dev, "devices", "speakers", s.speakersId, error) &&
           readString(dev, "devices", "output", s.outputId, error) &&
           readString(dev, "devices", "mic_name", s.micName, error) &&
           readString(dev, "devices", "speakers_name", s.speakersName, error) &&
           readString(dev, "devices", "output_name", s.outputName, error);
}

bool parseToml(std::string_view text, const char* what, toml::table& root, std::string& error) {
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::ostringstream oss;
        oss << what << " parse error: " << e;
        error = oss.str();
        return false;
    }
    return true;
}

bool readFile(const std::filesystem::path& path, std::string& text, std::string& error) {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open " + pathToUtf8(path);
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
    return true;
}

}  // namespace

bool parseConfig(std::string_view text, AppConfig& out, std::string& error) {
    toml::table root;
    if (!parseToml(text, "config", root, error)) return false;
    AppConfig cfg;
    checkKeys(root, "", {"format", "devices", "engine", "chain"}, cfg.warnings);

    const toml::table* fmt = nullptr;
    const toml::table* dev = nullptr;
    const toml::table* eng = nullptr;
    if (!tableAt(root, "format", fmt, error) || !tableAt(root, "devices", dev, error) ||
        !tableAt(root, "engine", eng, error)) {
        return false;
    }

    if (fmt) {
        checkKeys(*fmt, "format", {"sample_rate", "frame_ms", "mic_channels", "reference_channels"}, cfg.warnings);
        double frameMs = cfg.format.frameMs();
        if (!readUint(*fmt, "format", "sample_rate", 8000, 192000, cfg.format.sampleRate, error) ||
            !readNumber(*fmt, "format", "frame_ms", 1.0, 100.0, frameMs, error) ||
            !readUint(*fmt, "format", "mic_channels", 1, 8, cfg.format.micChannels, error) ||
            !readUint(*fmt, "format", "reference_channels", 1, 8, cfg.format.referenceChannels, error)) {
            return false;
        }
        cfg.format.frameSamples = uint32_t(std::lround(cfg.format.sampleRate * frameMs / 1000.0));
    }

    if (const toml::node* chainNode = root.get("chain")) {
        const toml::array* chain = chainNode->as_array();
        if (!chain) {
            error = "config: 'chain' must be an array of tables ([[chain]])";
            return false;
        }
        size_t index = 0;
        for (const toml::node& node : *chain) {
            const toml::table* t = node.as_table();
            if (!t) {
                error = "config: chain[" + std::to_string(index) + "] is not a table";
                return false;
            }
            StageConfig sc;
            if (auto id = (*t)["id"].value<std::string>()) {
                sc.id = *id;
            } else {
                error = "config: chain[" + std::to_string(index) + "] has no string 'id'";
                return false;
            }
            sc.params = *t;
            sc.params.erase("id");
            cfg.chain.push_back(std::move(sc));
            ++index;
        }
    }

    if (dev && !readDevices(*dev, cfg.engine, &cfg.warnings, error)) return false;
    if (eng) {
        checkKeys(*eng, "engine",
                  {"mic_raw", "reference_lead_ms", "output_buffer_ms", "output_render_ms", "output_channels",
                   "record_dir", "on_demand", "idle_stop_sec"},
                  cfg.warnings);
        EngineSettings& e = cfg.engine;
        if (!readBool(*eng, "engine", "mic_raw", e.micRaw, error) ||
            !readUint(*eng, "engine", "reference_lead_ms", 0, 500, e.referenceLeadMs, error) ||
            !readUint(*eng, "engine", "output_buffer_ms", 0, 500, e.outputBufferMs, error) ||
            !readUint(*eng, "engine", "output_render_ms", 0, 500, e.outputRenderMs, error) ||
            !readUint(*eng, "engine", "output_channels", 1, 8, e.outputChannels, error) ||
            !readString(*eng, "engine", "record_dir", e.recordDir, error) ||
            !readBool(*eng, "engine", "on_demand", e.onDemand, error) ||
            !readUint(*eng, "engine", "idle_stop_sec", 1, 3600, e.idleStopSec, error)) {
            return false;
        }
    }

    out = std::move(cfg);
    return true;
}

bool loadConfig(const std::filesystem::path& path, AppConfig& out, std::string& error) {
    std::string text;
    if (!readFile(path, text, error)) {
        error = "config: " + error;
        return false;
    }
    return parseConfig(text, out, error);
}

bool loadState(const std::filesystem::path& path, EngineSettings& settings, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;
    std::string text;
    toml::table root;
    const toml::table* dev = nullptr;
    if (!readFile(path, text, error) || !parseToml(text, "state", root, error) ||
        !tableAt(root, "devices", dev, error)) {
        return false;
    }
    if (!dev) return true;
    EngineSettings s = settings;
    if (!readDevices(*dev, s, nullptr, error)) return false;
    settings = std::move(s);
    return true;
}

bool saveState(const std::filesystem::path& path, const EngineSettings& settings, std::string& error) {
    toml::table devices;
    devices.insert_or_assign("mic", settings.micId);
    devices.insert_or_assign("speakers", settings.speakersId);
    devices.insert_or_assign("output", settings.outputId);
    devices.insert_or_assign("mic_name", settings.micName);
    devices.insert_or_assign("speakers_name", settings.speakersName);
    devices.insert_or_assign("output_name", settings.outputName);
    toml::table root;
    root.insert_or_assign("devices", std::move(devices));
    std::ostringstream oss;
    oss << "# Состояние bomboec: устройства, выбранные в меню трея. Пишет приложение.\n"
           "# Перекрывает [devices] из bomboec.toml; удалите файл, чтобы вернуться к конфигу.\n\n"
        << root << "\n";
    return writeFileAtomic(path, oss.str(), error);
}

bool writeFileAtomic(const std::filesystem::path& path, std::string_view text, std::string& error) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(text.data(), std::streamsize(text.size()));
            out.flush();
        }
        if (!out) {
            error = "cannot write " + pathToUtf8(tmp);
            out.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    // MSVC: MoveFileExW с MOVEFILE_REPLACE_EXISTING, прежний файл заменяется целиком.
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        error = "cannot replace " + pathToUtf8(path) + ": " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::string_view defaultConfigToml() {
    // default_config.inc генерирует src/core/CMakeLists.txt из config/default.toml.
    static constexpr std::string_view kText =
#include "default_config.inc"
        ;
    return kText;
}

}  // namespace bomboec
