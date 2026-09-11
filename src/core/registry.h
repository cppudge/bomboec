#pragma once

#include "core/stage.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bomboec {

// Реестр фабрик стадий по строковому id из конфига.
class StageRegistry {
public:
    using Factory = std::function<std::unique_ptr<IStage>()>;

    void add(std::string id, Factory factory) { factories_[std::move(id)] = std::move(factory); }

    std::unique_ptr<IStage> create(std::string_view id) const {
        auto it = factories_.find(std::string(id));
        return it == factories_.end() ? nullptr : it->second();
    }

    bool has(std::string_view id) const { return factories_.count(std::string(id)) != 0; }

    std::vector<std::string> ids() const {
        std::vector<std::string> out;
        for (const auto& [k, _] : factories_) out.push_back(k);
        return out;
    }

private:
    std::map<std::string, Factory> factories_;
};

}  // namespace bomboec
