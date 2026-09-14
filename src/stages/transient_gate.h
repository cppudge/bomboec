#pragma once

// Гейт импульсных помех (стук по столу, щелчок ручки, кружка): детектор атак и усиление по
// сэмплам. Общий для стадии transient (сама задерживает сигнал на lookahead) и стадии rnnoise
// (детектор смотрит на вход сети, усиление ложится на её выход: сеть и так задерживает звук на
// 20 ms, это покрывает lookahead, и своей задержки у гейта нет).
//
// Детектор. RNNoise громкие удары принимает за речь (VAD около 1) и пропускает почти нетронутыми,
// глухие удары с энергией ниже 200 Hz давит сам. На корпусе записей (docs/measurements.md)
// проходящие удары нарастают на 25-45 dB за 1 ms при пике около -10 dBFS, речь даже на взрывных
// согласных не быстрее 15-20 dB/ms. Поэтому: огибающая RMS по блокам 1 ms; скачок между
// соседними блоками >= rise_db_per_ms (или за два блока в 1.5 раза больше: APM размазывает атаку)
// при уровне блока >= level_dbfs - удар. Решает канал 0.
//
// Одной скорости атаки мало: на живой громкой речи (corpus/speech-call) она срабатывает на
// согласных. Различает их гармоничность окна вокруг атаки: максимум нормированной
// автокорреляции при лагах 2.5-15 ms (основной тон 67-400 Hz). На корпусе (2026-09-12) в окне
// [-20 ms, +4 ms] от атаки у 5 срабатываний на речи она 0.65-0.86, у 35 ударов и 34 щелчков
// клавиатуры не выше 0.27, так что порог 0.4 разделяет их с запасом. Окно только из прошлого
// ([-20 ms, 0]) удары и речь не разделяет (у ударов в нём остаётся гармоничная комната),
// поэтому проверке нужен lookahead_ms.
//
// Усиление, mode:
//   "hold" - падает до -depth_db за preroll_ms до атаки, держится hold_ms (отскоки и звон
//            стола) и возвращается за release_ms. Поверх речи гасит голос на всё удержание.
//   "duck" - прижимает огибающую удара (с заглядыванием на lookahead) к фону до атаки плюс
//            margin_db, не глубже depth_db: в тишине удар давится на всю глубину, поверх речи
//            только до уровня голоса, и усиление возвращается (за release_ms), как только удар
//            затух до фона. hold_ms - сколько после атаки гейт следит за огибающей (отскоки).
//            Фон - то, что слышно: выход стадии, если она его показывает (observeOutput, у
//            rnnoise это выход сети без шума, который она убрала), иначе вход.
//
// Ключи (у стадии rnnoise с префиксом gate_; значения по умолчанию у стадий свои, см. Defaults):
//   rise_db_per_ms      скачок огибающей за 1 ms
//   level_dbfs          ниже этого уровня атака не считается ударом
//   depth_db            глубина гейта
//   hold_ms, release_ms
//   harmonicity_max     гейт не срабатывает, если гармоничность окна выше; 1.0 = проверка выключена
//   lookahead_ms        будущее после атаки для окна гармоничности и огибающей duck
//   preroll_ms          сколько до атаки уже под гейтом
//   mode, margin_db

#include "core/audio_format.h"
#include "core/stage_params.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bomboec {

class TransientGate {
public:
    // Значения ключей по умолчанию и пределы, которые задаёт стадия.
    struct Defaults {
        const char* mode = "hold";
        double riseDbPerMs = 20.0;
        double levelDbfs = -30.0;
        double depthDb = 20.0;
        double holdMs = 60.0;
        double releaseMs = 100.0;
        double harmonicityMax = 1.0;
        double lookaheadMs = 0.0;
        double maxLookaheadMs = 8.0;
        double prerollMs = 2.0;
        double marginDb = 3.0;
        // Задержка выдаваемого усиления относительно входа analyze(). 0 = ровно lookahead
        // (стадия transient сама задерживает сигнал на столько же), иначе столько сэмплов
        // (стадия rnnoise: задержка сети); lookahead тогда не больше неё.
        uint32_t outputDelaySamples = 0;
    };

    // Читает ключи с префиксом prefix. При ошибке false и текст в error (без префикса стадии).
    bool init(const PipelineFormat& fmt, StageParams& cfg, std::string_view prefix, const Defaults& defaults,
              std::string& error);
    void reset();

    // Очередной кадр входа детектора (один канал). Затем gains() - усиление для кадра, который
    // отстаёт от этого входа на delaySamples().
    void analyze(const float* x);
    std::span<const float> gains() const { return std::span<const float>(line_).first(frameSamples_); }
    // Кадр выхода стадии с уже применённым gains() (один канал): фон для duck берётся по нему.
    void observeOutput(const float* y);
    float minGain() const { return minGain_; }  // минимум gains() (1 = гейт кадр не трогает)

    uint32_t lookaheadSamples() const { return look_ * block_; }
    uint32_t delaySamples() const { return pending_ + look_ * block_; }
    uint64_t triggers() const { return triggers_; }

private:
    void pushBlocks(const float* x);
    void markAttacks();
    bool voiced(uint32_t j);
    void buildGains();
    void preroll(size_t attackSample, float gain);
    float historyDb() const;

    enum class Mode : uint8_t { Hold, Duck };

    Mode mode_ = Mode::Hold;
    float riseDb_ = 20.0f, levelDb_ = -30.0f;
    float depthDb_ = 20.0f, depthGain_ = 0.1f, marginDb_ = 3.0f;
    float harmonicityMax_ = 1.0f;
    uint32_t holdSamples_ = 0, prerollSamples_ = 96;
    float releaseCoef_ = 0.0f, attackCoef_ = 1.0f;
    uint32_t frameSamples_ = 480, block_ = 48, blocks_ = 10, look_ = 0, pending_ = 0;
    uint32_t decPerBlock_ = 6, lagMin_ = 15, lagMax_ = 90;

    std::vector<float> seqDb_;         // уровни блоков: выпускаемые blocks_, затем look_ будущих
    std::vector<bool> attack_;         // атака на выпускаемом блоке
    std::vector<float> dec_, window_;  // децимированный вход и окно автокорреляции
    std::vector<float> line_;          // усиление: pending_ ждущих сэмплов, затем кадр
    std::vector<float> history_;       // мощность блоков до атаки (кольцо), фон для duck
    size_t historyPos_ = 0;
    bool outputHistory_ = false;  // history_ заполняет observeOutput, а не вход

    float prevDb_ = -120.0f;   // уровень блока перед выпускаемым окном
    float prevDb2_ = -120.0f;  // и блока перед ним
    bool prevAttack_ = false;  // атака на последнем выпущенном блоке
    float gain_ = 1.0f;        // усиление на конце кадра
    float minGain_ = 1.0f;
    uint32_t hold_ = 0;     // hold: сэмплов удержания; duck: сэмплов до предела hold_ms
    bool engaged_ = false;  // duck: удар идёт
    float bgDb_ = -120.0f;  // duck: фон до атаки
    uint64_t triggers_ = 0;
};

}  // namespace bomboec
