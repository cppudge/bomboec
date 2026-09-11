#include "core/config.h"

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
        cfg.format.frameSamples = uint32_t(cfg.format.sampleRate * frameMs / 1000.0 + 0.5);
        cfg.format.micChannels = uint32_t((*fmt)["mic_channels"].value_or(int64_t(1)));
        cfg.format.referenceChannels = uint32_t((*fmt)["reference_channels"].value_or(int64_t(2)));
    }
    if (cfg.format.sampleRate == 0 || cfg.format.frameSamples == 0 ||
        cfg.format.micChannels == 0 || cfg.format.referenceChannels == 0) {
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

    out = std::move(cfg);
    return true;
}

bool loadConfig(const std::filesystem::path& path, AppConfig& out, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "config: cannot open " + path.string();
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return parseConfig(ss.str(), out, error);
}

}  // namespace bomboec
