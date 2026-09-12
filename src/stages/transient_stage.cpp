// Гейт импульсных помех (стук по столу, щелчок ручки, кружка) по скорости атаки.
//
// RNNoise такие удары принимает за речь (VAD около 1) и пропускает почти нетронутыми, а
// глухие удары с энергией ниже 200 Hz давит сам. На корпусе записей (docs/measurements.md)
// проходящие удары нарастают на 25-45 dB за 1 ms при пике около -10 dBFS, речь даже на
// взрывных согласных не быстрее 15-20 dB/ms. Поэтому: огибающая RMS по блокам 1 ms; скачок
// между соседними блоками >= rise_db_per_ms при уровне блока >= level_dbfs - удар. Усиление
// падает до -depth_db за два блока до срабатывания (в пределах кадра, добавочной задержки
// нет), держится hold_ms (отскоки и звон стола) и возвращается за release_ms. Решает канал 0,
// применяется ко всем. Стоит после AEC: эхо ударных из колонок уже вычтено, остаток ниже
// level_dbfs.

#include "stages/builtin_stages.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace bomboec {
namespace {

class TransientStage final : public IStage {
public:
    StageInfo info() const override {
        return {"transient", fmt_.sampleRate, fmt_.frameSamples, capBit(Cap::Transient), 0};
    }

    bool init(const PipelineFormat& fmt, StageParams& cfg, std::string& error) override {
        fmt_ = fmt;
        riseDb_ = float(cfg.number("rise_db_per_ms", 20.0, 5.0, 60.0));
        levelDb_ = float(cfg.number("level_dbfs", -30.0, -80.0, 0.0));
        const double depthDb = cfg.number("depth_db", 20.0, 0.0, 60.0);
        const double holdMs = cfg.number("hold_ms", 60.0, 0.0, 1000.0);
        const double releaseMs = cfg.number("release_ms", 100.0, 1.0, 5000.0);
        if (!cfg.ok()) {
            error = "transient: " + cfg.error();
            return false;
        }
        block_ = fmt.sampleRate / 1000;
        if (block_ == 0 || fmt.frameSamples % block_ != 0) {
            error = "transient: the frame must hold a whole number of 1 ms blocks";
            return false;
        }
        blocks_ = fmt.frameSamples / block_;
        depthGain_ = float(std::pow(10.0, -depthDb / 20.0));
        holdSamples_ = uint32_t(holdMs / 1000.0 * fmt.sampleRate);
        releaseCoef_ = float(1.0 - std::exp(-1.0 / (releaseMs / 1000.0 * fmt.sampleRate)));
        blockDb_.assign(blocks_, -120.0f);
        attack_.assign(blocks_, false);
        gains_.assign(fmt.frameSamples, 1.0f);
        reset();
        return true;
    }

    void process(Frame& mic, const Frame*) override {
        if (mic.channels() == 0) return;
        const float* x = mic.channel(0).data();
        // 1. Огибающая блоков 1 ms и блоки срабатывания (с двумя блоками пре-ролла).
        std::fill(attack_.begin(), attack_.end(), false);
        float prev = prevDb_;
        for (uint32_t j = 0; j < blocks_; ++j) {
            double e = 0.0;
            for (uint32_t i = 0; i < block_; ++i) {
                const float v = x[size_t(j) * block_ + i];
                e += double(v) * v;
            }
            const float db = float(10.0 * std::log10(e / block_ + 1e-12));
            blockDb_[j] = db;
            if (db >= levelDb_ && db - prev >= riseDb_) {
                ++triggers_;
                for (uint32_t k = (j >= 2 ? j - 2 : 0); k <= j; ++k) attack_[k] = true;
            }
            prev = db;
        }
        prevDb_ = prev;
        // 2. Усиление по сэмплам: атака мгновенная, удержание, экспоненциальный release.
        float g = gain_;
        float minGain = 1.0f;
        for (uint32_t j = 0; j < blocks_; ++j) {
            if (attack_[j]) {
                g = depthGain_;
                hold_ = holdSamples_;
            }
            for (uint32_t i = 0; i < block_; ++i) {
                if (hold_ > 0) {
                    --hold_;
                } else {
                    g += (1.0f - g) * releaseCoef_;
                }
                gains_[size_t(j) * block_ + i] = g;
            }
            minGain = std::min(minGain, g);
        }
        gain_ = g;
        minGain_ = minGain;
        if (minGain >= 0.999f) return;  // гейт закрыт: кадр не трогаем
        for (uint32_t c = 0; c < mic.channels(); ++c) {
            float* y = mic.channel(c).data();
            for (uint32_t i = 0; i < fmt_.frameSamples; ++i) y[i] *= gains_[i];
        }
    }

    void reset() override {
        gain_ = 1.0f;
        minGain_ = 1.0f;
        hold_ = 0;
        prevDb_ = -120.0f;
        triggers_ = 0;
    }

    StageStats stats() const override {
        StageStats s;
        s.transients = triggers_;
        if (minGain_ < 0.999f) s.gainDb = 20.0 * std::log10(std::max(minGain_, 1e-6f));
        return s;
    }

private:
    PipelineFormat fmt_;
    float riseDb_ = 20.0f, levelDb_ = -30.0f;
    float depthGain_ = 0.1f;
    uint32_t holdSamples_ = 0;
    float releaseCoef_ = 0.0f;
    uint32_t block_ = 48, blocks_ = 10;
    std::vector<float> blockDb_, gains_;
    std::vector<bool> attack_;
    float prevDb_ = -120.0f;  // уровень последнего блока прошлого кадра
    float gain_ = 1.0f;       // усиление на конце кадра
    float minGain_ = 1.0f;    // минимум в последнем кадре (статус)
    uint32_t hold_ = 0;       // сэмплов удержания осталось
    uint64_t triggers_ = 0;
};

}  // namespace

std::unique_ptr<IStage> makeTransientStage() { return std::make_unique<TransientStage>(); }

}  // namespace bomboec
