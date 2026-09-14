// Стадия гейта импульсных помех (стук по столу, щелчок ручки, кружка). Детектор, режимы и ключи
// описаны в transient_gate.h; здесь сигнал задерживается на lookahead_ms, чтобы решение о блоке
// видело будущее после атаки. Решает канал 0, применяется ко всем. Стоит после AEC: эхо ударных
// из колонок уже вычтено, остаток ниже level_dbfs. Ключи по умолчанию (как до 2026-09-14):
//   mode = "hold", rise_db_per_ms = 20, level_dbfs = -30, depth_db = 20, hold_ms = 60,
//   release_ms = 100, harmonicity_max = 1.0 (проверка выключена), lookahead_ms = 0 (до 8),
//   preroll_ms = 2, margin_db = 3
//
// Тот же гейт без задержки есть в стадии rnnoise (gate = true): там детектор смотрит на вход
// сети, а её собственные 20 ms покрывают lookahead.

#include "stages/builtin_stages.h"
#include "stages/transient_gate.h"

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
        if (!gate_.init(fmt, cfg, "", TransientGate::Defaults{}, error)) {
            error = "transient: " + error;
            return false;
        }
        const uint32_t d = gate_.lookaheadSamples();
        delay_.assign(size_t(fmt.micChannels) * d, 0.0f);
        keep_.assign(d, 0.0f);
        return true;
    }

    void process(Frame& mic, const Frame*) override {
        if (mic.channels() == 0) return;
        gate_.analyze(mic.channel(0).data());
        const uint32_t n = fmt_.frameSamples;
        const uint32_t d = gate_.lookaheadSamples();
        const std::span<const float> gains = gate_.gains();
        for (uint32_t c = 0; c < mic.channels(); ++c) {
            float* y = mic.channel(c).data();
            // Выход отстаёт от входа на lookahead: сначала хвост прошлого кадра из delay_, затем
            // начало текущего; хвост текущего остаётся в delay_. Усиление считано по той же шкале.
            if (d != 0 && (size_t(c) + 1) * d <= delay_.size()) {
                float* tail = delay_.data() + size_t(c) * d;
                std::copy(tail, tail + d, keep_.begin());
                std::copy(y + (n - d), y + n, tail);
                std::copy_backward(y, y + (n - d), y + n);
                std::copy(keep_.begin(), keep_.begin() + d, y);
            }
            if (gate_.minGain() < 0.999f) {
                for (uint32_t i = 0; i < n; ++i) y[i] *= gains[i];
            }
        }
    }

    void reset() override {
        gate_.reset();
        std::fill(delay_.begin(), delay_.end(), 0.0f);
    }

    StageStats stats() const override {
        StageStats s;
        s.transients = gate_.triggers();
        if (gate_.minGain() < 0.999f) s.gainDb = 20.0 * std::log10(std::max(gate_.minGain(), 1e-6f));
        return s;
    }

private:
    PipelineFormat fmt_;
    TransientGate gate_;
    std::vector<float> delay_, keep_;
};

}  // namespace

std::unique_ptr<IStage> makeTransientStage() { return std::make_unique<TransientStage>(); }

}  // namespace bomboec
