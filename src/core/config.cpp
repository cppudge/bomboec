#include "core/config.h"

#include "core/utf8.h"

#include <cmath>
#include <fstream>
#include <sstream>

namespace bomboec {

bool parseConfig(std::string_view text, AppConfig& out, std::string& error) {
    toml::table root;
    try {
        root = toml::parse(text);
    } catch (const toml::parse_error& e) {
        std::ostringstream oss;
        oss << "config parse error: " << e;
        error = oss.str();
        return false;
    }
    AppConfig cfg;

    if (const toml::table* fmt = root["format"].as_table()) {
        cfg.format.sampleRate = uint32_t((*fmt)["sample_rate"].value_or(int64_t(48000)));
        const double frameMs = (*fmt)["frame_ms"].value_or(10.0);
        cfg.format.frameSamples = uint32_t(std::lround(cfg.format.sampleRate * frameMs / 1000.0));
        cfg.format.micChannels = uint32_t((*fmt)["mic_channels"].value_or(int64_t(1)));
        cfg.format.referenceChannels = uint32_t((*fmt)["reference_channels"].value_or(int64_t(2)));
    }
    if (cfg.format.sampleRate == 0 || cfg.format.frameSamples == 0 || cfg.format.micChannels == 0 ||
        cfg.format.referenceChannels == 0) {
        error = "config: format fields must be positive";
        return false;
    }

    if (const toml::array* chain = root["chain"].as_array()) {
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

    if (const toml::table* dev = root["devices"].as_table()) {
        cfg.engine.micId = (*dev)["mic"].value_or(std::string());
        cfg.engine.speakersId = (*dev)["speakers"].value_or(std::string());
        cfg.engine.outputId = (*dev)["output"].value_or(std::string());
        cfg.engine.micName = (*dev)["mic_name"].value_or(std::string());
        cfg.engine.speakersName = (*dev)["speakers_name"].value_or(std::string());
        cfg.engine.outputName = (*dev)["output_name"].value_or(std::string());
    }
    if (const toml::table* eng = root["engine"].as_table()) {
        cfg.engine.micRaw = (*eng)["mic_raw"].value_or(true);
        cfg.engine.referenceLeadMs = uint32_t((*eng)["reference_lead_ms"].value_or(int64_t(20)));
        cfg.engine.outputBufferMs = uint32_t((*eng)["output_buffer_ms"].value_or(int64_t(10)));
        cfg.engine.outputRenderMs = uint32_t((*eng)["output_render_ms"].value_or(int64_t(20)));
        cfg.engine.outputChannels = uint32_t((*eng)["output_channels"].value_or(int64_t(2)));
        cfg.engine.recordDir = (*eng)["record_dir"].value_or(std::string());
    }
    if (cfg.engine.outputChannels == 0 || cfg.engine.outputChannels > 8) {
        error = "config: engine.output_channels must be 1..8";
        return false;
    }

    cfg.raw = root;
    out = std::move(cfg);
    return true;
}

bool saveConfig(const std::filesystem::path& path, const AppConfig& cfg, std::string& error) {
    toml::table root = cfg.raw;
    toml::table devices;
    devices.insert_or_assign("mic", cfg.engine.micId);
    devices.insert_or_assign("speakers", cfg.engine.speakersId);
    devices.insert_or_assign("output", cfg.engine.outputId);
    devices.insert_or_assign("mic_name", cfg.engine.micName);
    devices.insert_or_assign("speakers_name", cfg.engine.speakersName);
    devices.insert_or_assign("output_name", cfg.engine.outputName);
    root.insert_or_assign("devices", std::move(devices));

    toml::table* eng = root["engine"].as_table();
    if (!eng) {
        root.insert_or_assign("engine", toml::table{});
        eng = root["engine"].as_table();
    }
    eng->insert_or_assign("mic_raw", cfg.engine.micRaw);
    eng->insert_or_assign("reference_lead_ms", int64_t(cfg.engine.referenceLeadMs));
    eng->insert_or_assign("output_buffer_ms", int64_t(cfg.engine.outputBufferMs));
    eng->insert_or_assign("output_render_ms", int64_t(cfg.engine.outputRenderMs));
    eng->insert_or_assign("output_channels", int64_t(cfg.engine.outputChannels));
    eng->insert_or_assign("record_dir", cfg.engine.recordDir);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "config: cannot write " + pathToUtf8(path);
        return false;
    }
    out << root << "\n";
    return bool(out);
}

bool loadConfig(const std::filesystem::path& path, AppConfig& out, std::string& error) {
    const std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "config: cannot open " + pathToUtf8(path);
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return parseConfig(ss.str(), out, error);
}

std::string_view defaultConfigToml() {
    // default_config.inc генерирует src/core/CMakeLists.txt из config/default.toml.
    static constexpr std::string_view kText =
#include "default_config.inc"
        ;
    return kText;
}

}  // namespace bomboec
