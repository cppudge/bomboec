#include "core/stage_params.h"

#include <algorithm>
#include <optional>
#include <sstream>

namespace bomboec {

const toml::node* StageParams::fetch(std::string_view key) {
    if (std::find(used_.begin(), used_.end(), key) == used_.end()) used_.emplace_back(key);
    return table_ ? table_->get(key) : nullptr;
}

void StageParams::fail(std::string_view key, const std::string& what) {
    if (!error_.empty()) return;  // первая ошибка понятнее списка
    error_ = std::string(key) + " " + what;
}

bool StageParams::boolean(std::string_view key, bool defaultValue) {
    const toml::node* n = fetch(key);
    if (!n) return defaultValue;
    const std::optional<bool> v = n->value_exact<bool>();
    if (!v) {
        fail(key, "must be true or false");
        return defaultValue;
    }
    return *v;
}

int64_t StageParams::integer(std::string_view key, int64_t defaultValue, int64_t lo, int64_t hi) {
    const toml::node* n = fetch(key);
    if (!n) return defaultValue;
    const std::optional<int64_t> v = n->value_exact<int64_t>();
    if (!v || *v < lo || *v > hi) {
        fail(key, "must be an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
        return defaultValue;
    }
    return *v;
}

double StageParams::number(std::string_view key, double defaultValue, double lo, double hi) {
    const toml::node* n = fetch(key);
    if (!n) return defaultValue;
    const std::optional<double> v = n->value<double>();  // целое тоже подходит
    if (!v || !(*v >= lo && *v <= hi)) {
        std::ostringstream oss;
        oss << "must be a number in " << lo << ".." << hi;
        fail(key, oss.str());
        return defaultValue;
    }
    return *v;
}

std::string StageParams::string(std::string_view key, std::string_view defaultValue) {
    const toml::node* n = fetch(key);
    if (!n) return std::string(defaultValue);
    const std::optional<std::string> v = n->value_exact<std::string>();
    if (!v) {
        fail(key, "must be a string");
        return std::string(defaultValue);
    }
    return *v;
}

std::string StageParams::choice(std::string_view key, std::string_view defaultValue,
                                std::initializer_list<std::string_view> allowed) {
    std::string v = string(key, defaultValue);
    if (std::find(allowed.begin(), allowed.end(), v) != allowed.end()) return v;
    std::string list;
    for (const std::string_view a : allowed) list += (list.empty() ? "" : "|") + std::string(a);
    fail(key, "must be " + list);
    return std::string(defaultValue);
}

std::vector<std::string> StageParams::unknownKeys() const {
    std::vector<std::string> out;
    if (!table_) return out;
    for (const auto& entry : *table_) {
        const std::string_view key = entry.first.str();
        if (std::find(used_.begin(), used_.end(), key) == used_.end()) out.emplace_back(key);
    }
    return out;
}

}  // namespace bomboec
