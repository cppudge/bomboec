#pragma once

#include "core/config.h"
#include "core/registry.h"
#include "core/stage.h"

#include <memory>
#include <string>
#include <vector>

namespace bomboec {

// Последовательность стадий. Проверяет совместимость формата и уникальность
// возможностей, суммирует задержку, отдаёт агрегированную статистику.
class Chain {
public:
    void add(std::unique_ptr<IStage> stage, toml::table params = {});

    // Инициализирует все стадии. При ошибке false и текст в error. Ключи стадий,
    // которые ни одна из них не прочитала (опечатки), попадают в warnings().
    bool init(const PipelineFormat& fmt, std::string& error);
    const std::vector<std::string>& warnings() const { return warnings_; }

    void process(Frame& mic, const Frame* reference);
    void reset();

    size_t size() const { return entries_.size(); }
    const IStage& stage(size_t i) const { return *entries_[i].stage; }
    IStage& stage(size_t i) { return *entries_[i].stage; }

    uint32_t caps() const { return caps_; }
    uint32_t latencyFrames() const { return latencyFrames_; }

    // Статистика: первое непустое значение по каждому полю в порядке стадий.
    StageStats stats() const;

private:
    struct Entry {
        std::unique_ptr<IStage> stage;
        toml::table params;
    };
    std::vector<Entry> entries_;
    uint32_t caps_ = 0;
    uint32_t latencyFrames_ = 0;
    std::vector<std::string> warnings_;
};

// Собирает цепочку по конфигу через реестр (без init).
std::unique_ptr<Chain> buildChain(const StageRegistry& registry, const AppConfig& cfg, std::string& error);

}  // namespace bomboec
