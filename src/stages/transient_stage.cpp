// Гейт импульсных помех (стук по столу, щелчок ручки, кружка) по скорости атаки.
//
// RNNoise такие удары принимает за речь (VAD около 1) и пропускает почти нетронутыми, а
// глухие удары с энергией ниже 200 Hz давит сам. На корпусе записей (docs/measurements.md)
// проходящие удары нарастают на 25-45 dB за 1 ms при пике около -10 dBFS, речь даже на
// взрывных согласных не быстрее 15-20 dB/ms. Поэтому: огибающая RMS по блокам 1 ms; скачок
// между соседними блоками >= rise_db_per_ms при уровне блока >= level_dbfs - удар. Усиление
// падает до -depth_db за два блока до срабатывания, держится hold_ms (отскоки и звон стола)
// и возвращается за release_ms. Решает канал 0, применяется ко всем. Стоит после AEC: эхо
// ударных из колонок уже вычтено, остаток ниже level_dbfs.
//
// Одной скорости атаки мало: на живой громкой речи (corpus/speech-call) она срабатывает на
// согласных. Различает их гармоничность окна вокруг атаки: максимум нормированной
// автокорреляции при лагах 2.5-15 ms (основной тон 67-400 Hz). На корпусе (2026-09-12) в окне
// [-20 ms, +4 ms] от атаки у 5 срабатываний на речи она 0.65-0.86, у 35 ударов и 34 щелчков
// клавиатуры не выше 0.27, так что порог 0.4 разделяет их с запасом. Нужные 4 ms после атаки
// дают задержку стадии lookahead_ms: решение принимается о блоке, который на lookahead_ms
// отстаёт от входа. Окно только из прошлого ([-20 ms, 0]) удары и речь не разделяет (у ударов
// в нём остаётся гармоничная комната), поэтому без lookahead_ms проверка бессмысленна.
//
// Ключи конфига:
//   rise_db_per_ms = 20   скачок огибающей за 1 ms
//   level_dbfs = -30      ниже этого уровня атака не считается ударом
//   depth_db = 20         глубина гейта
//   hold_ms = 60, release_ms = 100
//   harmonicity_max = 1.0 гейт не срабатывает, если гармоничность окна выше; 1.0 = проверка
//                         выключена (поведение до 2026-09-12)
//   lookahead_ms = 0      задержка стадии ради окна после атаки; без неё harmonicity_max
//                         работать не будет

#include "stages/builtin_stages.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace bomboec {
namespace {

// Децимация для автокорреляции: 48 kHz / 8 = 6 kHz, основного тона хватает с избытком.
constexpr uint32_t kDecimation = 8;
// Окно гармоничности до атаки, ms.
constexpr uint32_t kHarmonicityPastMs = 20;
// Лаги автокорреляции, ms (основной тон 400 ... 67 Hz).
constexpr double kPitchMinMs = 2.5, kPitchMaxMs = 15.0;

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
        harmonicityMax_ = float(cfg.number("harmonicity_max", 1.0, 0.0, 1.0));
        const double lookaheadMs = cfg.number("lookahead_ms", 0.0, 0.0, 8.0);
        if (!cfg.ok()) {
            error = "transient: " + cfg.error();
            return false;
        }
        block_ = fmt.sampleRate / 1000;
        if (block_ == 0 || fmt.frameSamples % block_ != 0 || block_ % kDecimation != 0) {
            error = "transient: the frame must hold a whole number of 1 ms blocks";
            return false;
        }
        blocks_ = fmt.frameSamples / block_;
        look_ = uint32_t(lookaheadMs);
        if (look_ >= blocks_) look_ = blocks_ - 1;  // задержка меньше кадра: хвост влезает в delay_
        depthGain_ = float(std::pow(10.0, -depthDb / 20.0));
        holdSamples_ = uint32_t(holdMs / 1000.0 * fmt.sampleRate);
        releaseCoef_ = float(1.0 - std::exp(-1.0 / (releaseMs / 1000.0 * fmt.sampleRate)));
        decPerBlock_ = block_ / kDecimation;
        lagMin_ = uint32_t(kPitchMinMs * decPerBlock_);
        lagMax_ = uint32_t(kPitchMaxMs * decPerBlock_);
        seqDb_.assign(size_t(blocks_) + look_, -120.0f);
        attack_.assign(blocks_, false);
        gains_.assign(fmt.frameSamples, 1.0f);
        dec_.assign((size_t(blocks_) + look_ + kHarmonicityPastMs) * decPerBlock_, 0.0f);
        window_.assign((size_t(kHarmonicityPastMs) + look_) * decPerBlock_, 0.0f);
        delay_.assign(size_t(fmt.micChannels) * look_ * block_, 0.0f);
        keep_.assign(size_t(look_) * block_, 0.0f);
        reset();
        return true;
    }

    void process(Frame& mic, const Frame*) override {
        if (mic.channels() == 0) return;
        const float* x = mic.channel(0).data();
        pushBlocks(x);
        markAttacks();
        buildGains();
        applyGains(mic);
    }

    void reset() override {
        gain_ = 1.0f;
        minGain_ = 1.0f;
        hold_ = 0;
        prevDb_ = -120.0f;
        triggers_ = 0;
        std::fill(seqDb_.begin(), seqDb_.end(), -120.0f);
        std::fill(dec_.begin(), dec_.end(), 0.0f);
        std::fill(delay_.begin(), delay_.end(), 0.0f);
    }

    StageStats stats() const override {
        StageStats s;
        s.transients = triggers_;
        if (minGain_ < 0.999f) s.gainDb = 20.0 * std::log10(std::max(minGain_, 1e-6f));
        return s;
    }

private:
    // Уровни блоков кадра в конец seqDb_ (перед ними - блоки, ещё не выпущенные из-за look_)
    // и децимированные сэмплы в конец dec_.
    void pushBlocks(const float* x) {
        std::copy(seqDb_.begin() + blocks_, seqDb_.end(), seqDb_.begin());
        for (uint32_t j = 0; j < blocks_; ++j) {
            double e = 0.0;
            for (uint32_t i = 0; i < block_; ++i) {
                const float v = x[size_t(j) * block_ + i];
                e += double(v) * v;
            }
            seqDb_[size_t(look_) + j] = float(10.0 * std::log10(e / block_ + 1e-12));
        }
        if (harmonicityMax_ >= 1.0f) return;  // автокорреляция не нужна
        const size_t frameDec = size_t(blocks_) * decPerBlock_;
        std::copy(dec_.begin() + std::ptrdiff_t(frameDec), dec_.end(), dec_.begin());
        float* out = dec_.data() + (dec_.size() - frameDec);
        for (size_t k = 0; k < frameDec; ++k) {
            float sum = 0.0f;
            for (uint32_t i = 0; i < kDecimation; ++i) sum += x[k * kDecimation + i];
            out[k] = sum / float(kDecimation);
        }
    }

    // Атаки на выпускаемых блоках seqDb_[0 .. blocks_-1]: вход опережает выход на look_ блоков,
    // поэтому у каждого из них есть look_ ms будущего для окна гармоничности.
    void markAttacks() {
        std::fill(attack_.begin(), attack_.end(), false);
        float prev = prevDb_;
        for (uint32_t j = 0; j < blocks_; ++j) {
            const float db = seqDb_[j];
            if (db >= levelDb_ && db - prev >= riseDb_ && !voiced(j)) {
                ++triggers_;
                for (uint32_t k = (j >= 2 ? j - 2 : 0); k <= j; ++k) attack_[k] = true;
            }
            prev = db;
        }
        prevDb_ = prev;
    }

    // Гармоничность окна [-kHarmonicityPastMs, +look_] вокруг блока j: максимум нормированной
    // автокорреляции на лагах основного тона. true = голос, гейту не срабатывать.
    bool voiced(uint32_t j) {
        if (harmonicityMax_ >= 1.0f || look_ == 0) return false;
        const size_t len = window_.size();
        const size_t end = dec_.size() - size_t(blocks_ - j) * decPerBlock_;
        if (end < len) return false;
        double mean = 0.0;
        for (size_t i = 0; i < len; ++i) mean += dec_[end - len + i];
        mean /= double(len);
        double energy = 0.0;
        for (size_t i = 0; i < len; ++i) {
            window_[i] = float(dec_[end - len + i] - mean);
            energy += double(window_[i]) * window_[i];
        }
        if (energy <= 0.0) return false;
        double best = 0.0;
        for (size_t lag = lagMin_; lag <= lagMax_ && lag < len; ++lag) {
            double sum = 0.0;
            for (size_t i = 0; i + lag < len; ++i) sum += double(window_[i]) * window_[i + lag];
            best = std::max(best, sum);
        }
        return best / energy > double(harmonicityMax_);
    }

    // Усиление по сэмплам: атака мгновенная, удержание, экспоненциальный release.
    void buildGains() {
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
    }

    // Выход отстаёт от входа на look_ блоков: сначала хвост прошлого кадра из delay_, затем
    // начало текущего; хвост текущего остаётся в delay_. Усиление считано по той же шкале.
    void applyGains(Frame& mic) {
        const uint32_t n = fmt_.frameSamples;
        const uint32_t d = look_ * block_;
        for (uint32_t c = 0; c < mic.channels(); ++c) {
            float* y = mic.channel(c).data();
            if (d != 0 && (size_t(c) + 1) * d <= delay_.size()) {
                float* tail = delay_.data() + size_t(c) * d;
                std::copy(tail, tail + d, keep_.begin());
                std::copy(y + (n - d), y + n, tail);
                std::copy_backward(y, y + (n - d), y + n);
                std::copy(keep_.begin(), keep_.begin() + d, y);
            }
            if (minGain_ < 0.999f) {
                for (uint32_t i = 0; i < n; ++i) y[i] *= gains_[i];
            }
        }
    }

    PipelineFormat fmt_;
    float riseDb_ = 20.0f, levelDb_ = -30.0f;
    float depthGain_ = 0.1f;
    float harmonicityMax_ = 1.0f;
    uint32_t holdSamples_ = 0;
    float releaseCoef_ = 0.0f;
    uint32_t block_ = 48, blocks_ = 10, look_ = 0;
    uint32_t decPerBlock_ = 6, lagMin_ = 15, lagMax_ = 90;
    std::vector<float> seqDb_;  // уровни блоков: look_ отложенных, затем blocks_ новых
    std::vector<float> gains_, dec_, window_, delay_, keep_;
    std::vector<bool> attack_;
    float prevDb_ = -120.0f;  // уровень блока перед выпускаемым окном
    float gain_ = 1.0f;       // усиление на конце кадра
    float minGain_ = 1.0f;    // минимум в последнем кадре (статус)
    uint32_t hold_ = 0;       // сэмплов удержания осталось
    uint64_t triggers_ = 0;
};

}  // namespace

std::unique_ptr<IStage> makeTransientStage() { return std::make_unique<TransientStage>(); }

}  // namespace bomboec
