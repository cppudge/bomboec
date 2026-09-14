#include "stages/transient_gate.h"

#include <algorithm>
#include <cmath>

namespace bomboec {
namespace {

// Децимация для автокорреляции: 48 kHz / 8 = 6 kHz, основного тона хватает с избытком.
constexpr uint32_t kDecimation = 8;
// Окно гармоничности до атаки, ms.
constexpr uint32_t kHarmonicityPastMs = 20;
// Лаги автокорреляции, ms (основной тон 400 ... 67 Hz).
constexpr double kPitchMinMs = 2.5, kPitchMaxMs = 15.0;
// duck: фон до атаки усредняется за столько блоков (ms).
constexpr size_t kHistoryBlocks = 20;
// Порог скачка за два блока относительно rise_db_per_ms (см. markAttacks).
constexpr float kRise2Ratio = 1.5f;
// duck: постоянная времени, с которой усиление догоняет огибающую вниз, ms.
constexpr double kDuckAttackMs = 0.3;

}  // namespace

bool TransientGate::init(const PipelineFormat& fmt, StageParams& cfg, std::string_view prefix, const Defaults& defaults,
                         std::string& error) {
    auto key = [&](std::string_view name) { return std::string(prefix) + std::string(name); };
    const Defaults& d = defaults;
    riseDb_ = float(cfg.number(key("rise_db_per_ms"), d.riseDbPerMs, 5.0, 60.0));
    levelDb_ = float(cfg.number(key("level_dbfs"), d.levelDbfs, -80.0, 0.0));
    depthDb_ = float(cfg.number(key("depth_db"), d.depthDb, 0.0, 60.0));
    const double holdMs = cfg.number(key("hold_ms"), d.holdMs, 0.0, 1000.0);
    const double releaseMs = cfg.number(key("release_ms"), d.releaseMs, 1.0, 5000.0);
    harmonicityMax_ = float(cfg.number(key("harmonicity_max"), d.harmonicityMax, 0.0, 1.0));
    const double lookaheadMs = cfg.number(key("lookahead_ms"), d.lookaheadMs, 0.0, d.maxLookaheadMs);
    const double prerollMs = cfg.number(key("preroll_ms"), d.prerollMs, 0.0, 10.0);
    const std::string mode = cfg.choice(key("mode"), d.mode, {"hold", "duck"});
    marginDb_ = float(cfg.number(key("margin_db"), d.marginDb, 0.0, 40.0));
    if (!cfg.ok()) {
        error = cfg.error();
        return false;
    }
    mode_ = mode == "duck" ? Mode::Duck : Mode::Hold;
    frameSamples_ = fmt.frameSamples;
    block_ = fmt.sampleRate / 1000;
    if (block_ == 0 || fmt.frameSamples % block_ != 0 || block_ % kDecimation != 0) {
        error = "the frame must hold a whole number of 1 ms blocks";
        return false;
    }
    blocks_ = fmt.frameSamples / block_;
    look_ = uint32_t(lookaheadMs);
    if (d.outputDelaySamples == 0) {
        if (look_ >= blocks_) look_ = blocks_ - 1;  // стадия задерживает сигнал внутри кадра
        pending_ = 0;
    } else {
        look_ = std::min(look_, d.outputDelaySamples / block_);
        pending_ = d.outputDelaySamples - look_ * block_;
    }
    depthGain_ = float(std::pow(10.0, -double(depthDb_) / 20.0));
    holdSamples_ = uint32_t(holdMs / 1000.0 * fmt.sampleRate);
    prerollSamples_ = uint32_t(prerollMs / 1000.0 * fmt.sampleRate);
    releaseCoef_ = float(1.0 - std::exp(-1.0 / (releaseMs / 1000.0 * fmt.sampleRate)));
    attackCoef_ = float(1.0 - std::exp(-1.0 / (kDuckAttackMs / 1000.0 * fmt.sampleRate)));
    decPerBlock_ = block_ / kDecimation;
    lagMin_ = uint32_t(kPitchMinMs * decPerBlock_);
    lagMax_ = uint32_t(kPitchMaxMs * decPerBlock_);
    seqDb_.assign(size_t(blocks_) + look_, -120.0f);
    attack_.assign(blocks_, false);
    dec_.assign((size_t(blocks_) + look_ + kHarmonicityPastMs) * decPerBlock_, 0.0f);
    window_.assign((size_t(kHarmonicityPastMs) + look_) * decPerBlock_, 0.0f);
    line_.assign(size_t(pending_) + fmt.frameSamples, 1.0f);
    history_.assign(kHistoryBlocks, 1e-12f);
    reset();
    return true;
}

void TransientGate::reset() {
    gain_ = 1.0f;
    minGain_ = 1.0f;
    hold_ = 0;
    prevDb_ = -120.0f;
    prevDb2_ = -120.0f;
    prevAttack_ = false;
    engaged_ = false;
    bgDb_ = -120.0f;
    triggers_ = 0;
    std::fill(seqDb_.begin(), seqDb_.end(), -120.0f);
    std::fill(dec_.begin(), dec_.end(), 0.0f);
    std::fill(line_.begin(), line_.end(), 1.0f);
    std::fill(history_.begin(), history_.end(), 1e-12f);
    historyPos_ = 0;
}

void TransientGate::observeOutput(const float* y) {
    outputHistory_ = true;
    if (mode_ != Mode::Duck) return;
    for (uint32_t j = 0; j < blocks_; ++j) {
        float e = 0.0f;
        for (uint32_t i = 0; i < block_; ++i) e += y[size_t(j) * block_ + i] * y[size_t(j) * block_ + i];
        history_[historyPos_] = e / float(block_);
        historyPos_ = (historyPos_ + 1) % history_.size();
    }
}

void TransientGate::analyze(const float* x) {
    pushBlocks(x);
    markAttacks();
    buildGains();
}

// Уровни блоков кадра в конец seqDb_ (перед ними - блоки, ещё не выпущенные из-за look_)
// и децимированные сэмплы в конец dec_.
void TransientGate::pushBlocks(const float* x) {
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

// Атаки на выпускаемых блоках seqDb_[0 .. blocks_-1]: вход опережает их на look_ блоков,
// поэтому у каждого из них есть look_ ms будущего для окна гармоничности.
//
// Скачок считается и за два блока (порог в kRise2Ratio раз выше): гейт стоит после WebRTC APM,
// а его банк фильтров и подавитель размазывают атаку удара на 2 ms. На корпусе (2026-09-14) самый
// громкий удар после APM шёл -52 -> -31 -> -13 dBFS: первый шаг ниже level_dbfs, второй 18 dB,
// и гейт его пропускал. Скачок за 2 ms >= 30 dB на выходе APM добавил 1 удар из 52 и 2 щелчка
// клавиатуры из 44, а на речи (speech, speech-call) ни одного срабатывания без гармоничности.
void TransientGate::markAttacks() {
    std::fill(attack_.begin(), attack_.end(), false);
    float prev = prevDb_, prev2 = prevDb2_;
    bool prevAttack = prevAttack_;  // скачок за два блока через уже сработавший блок - та же атака
    for (uint32_t j = 0; j < blocks_; ++j) {
        const float db = seqDb_[j];
        const bool rise = db - prev >= riseDb_ || (!prevAttack && db - prev2 >= riseDb_ * kRise2Ratio);
        prevAttack = db >= levelDb_ && rise && !voiced(j);
        if (prevAttack) {
            ++triggers_;
            attack_[j] = true;
        }
        prev2 = prev;
        prev = db;
    }
    prevDb_ = prev;
    prevDb2_ = prev2;
    prevAttack_ = prevAttack;
}

// Гармоничность окна [-kHarmonicityPastMs, +look_] вокруг блока j: максимум нормированной
// автокорреляции на лагах основного тона. true = голос, гейту не срабатывать.
bool TransientGate::voiced(uint32_t j) {
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

// Пре-ролл: сэмплы перед атакой, ещё не выданные (в этом кадре или ждущие в line_), не громче gain.
void TransientGate::preroll(size_t attackSample, float gain) {
    const size_t from = attackSample > prerollSamples_ ? attackSample - prerollSamples_ : 0;
    for (size_t s = from; s < attackSample; ++s) line_[s] = std::min(line_[s], gain);
}

float TransientGate::historyDb() const {
    double sum = 0.0;
    for (const float p : history_) sum += p;
    return float(10.0 * std::log10(sum / double(history_.size()) + 1e-12));
}

// Усиление по сэмплам в конец line_ (после pending_ ждущих): атака мгновенная, дальше hold или duck.
void TransientGate::buildGains() {
    std::copy(line_.begin() + frameSamples_, line_.end(), line_.begin());
    float g = gain_;
    for (uint32_t j = 0; j < blocks_; ++j) {
        const size_t t = size_t(pending_) + size_t(j) * block_;
        float* out = line_.data() + t;
        if (mode_ == Mode::Hold) {
            if (attack_[j]) {
                g = depthGain_;
                hold_ = holdSamples_;
                preroll(t, g);
            }
            for (uint32_t i = 0; i < block_; ++i) {
                if (hold_ > 0) {
                    --hold_;
                } else {
                    g += (1.0f - g) * releaseCoef_;
                }
                out[i] = g;
            }
            continue;
        }

        if (attack_[j]) {
            if (!engaged_) bgDb_ = historyDb();
            engaged_ = true;
            hold_ = holdSamples_;
        }
        float target = 1.0f;
        if (engaged_) {
            float env = seqDb_[j];
            for (uint32_t k = 1; k <= look_; ++k) env = std::max(env, seqDb_[size_t(j) + k]);
            const float over = env - (bgDb_ + marginDb_);
            if (over > 0.0f) target = float(std::pow(10.0, -double(std::min(over, depthDb_)) / 20.0));
            if (hold_ <= block_) engaged_ = false;
            else hold_ -= block_;
            if (attack_[j]) {
                g = std::min(g, target);
                preroll(t, g);
            }
        } else if (!outputHistory_) {
            history_[historyPos_] = float(std::pow(10.0, double(seqDb_[j]) / 10.0));
            historyPos_ = (historyPos_ + 1) % history_.size();
        }
        for (uint32_t i = 0; i < block_; ++i) {
            g += (target - g) * (target < g ? attackCoef_ : releaseCoef_);
            out[i] = g;
        }
    }
    gain_ = g;
    minGain_ = *std::min_element(line_.begin(), line_.begin() + frameSamples_);
}

}  // namespace bomboec
