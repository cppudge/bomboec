#pragma once

#include "core/audio_format.h"
#include "core/frame.h"
#include "core/stage_params.h"

#include <cstdint>
#include <optional>
#include <string>

namespace bomboec {

// Возможности стадии. Цепочка следит, чтобы каждая была объявлена не более одного раза.
enum class Cap : uint32_t {
    Hpf = 1u << 0,
    Aec = 1u << 1,
    Ns = 1u << 2,
    Agc = 1u << 3,
    Limiter = 1u << 4,
};

constexpr uint32_t capBit(Cap c) { return static_cast<uint32_t>(c); }
constexpr bool hasCap(uint32_t caps, Cap c) { return (caps & capBit(c)) != 0; }
const char* capName(Cap c);

struct StageInfo {
    std::string id;
    uint32_t sampleRate = 48000;
    uint32_t frameSamples = 480;
    uint32_t caps = 0;
    uint32_t latencyFrames = 0;  // алгоритмическая задержка стадии в кадрах
};

// Диагностика стадии. Поля заполняются теми стадиями, у которых они есть.
// Тривиально копируемая: Pipeline публикует снимок через SeqLock.
struct StageStats {
    std::optional<double> delayMs;
    std::optional<double> erlDb;
    std::optional<double> erleDb;
    std::optional<double> residualEchoLikelihood;
    std::optional<double> gainDb;
    uint64_t errors = 0;  // кадров, которые бэкенд вернул с ошибкой (Chain суммирует)
};

class IStage {
public:
    virtual ~IStage() = default;

    virtual StageInfo info() const = 0;

    // Инициализация под формат конвейера. При ошибке возвращает false и
    // текст в error. Аллокации допустимы только здесь и в reset(). Все ключи
    // конфига читаются через cfg: неверный тип или диапазон - ошибка (cfg.ok()),
    // непрочитанные ключи Chain считает опечатками.
    virtual bool init(const PipelineFormat& fmt, StageParams& cfg, std::string& error) = 0;

    // mic обрабатывается in-place. reference уже выровнен движком относительно
    // mic; для стадий без Cap::Aec может быть nullptr.
    virtual void process(Frame& mic, const Frame* reference) = 0;

    // Сброс внутреннего состояния (смена устройства, разрыв потока).
    virtual void reset() = 0;

    // process(), reset() и stats() вызываются из одного аудиопотока; окно статуса
    // видит снимок, который публикует Pipeline. Поэтому стадии не нужна синхронизация.
    virtual StageStats stats() const = 0;
};

}  // namespace bomboec
