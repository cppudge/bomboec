// Стадия шумоподавления на RNNoise (xiph, 0.2): рекуррентная сеть считает усиление по
// 22 полосам на кадр 10 ms при 48 kHz, лучше NS из WebRTC на нестационарных шумах
// (клавиатура, щелчки). Алгоритмическая задержка один кадр (перекрытие окон анализа).
//
// Ключей конфига нет. Пробовалась смесь с сухим сигналом (mix): даже с выравниванием по
// кадру она теряла 2 dB речи (сеть меняет фазу), и на корпусе выигрыша не дала.
//
// Заголовок rnnoise не выходит за пределы этого файла.

#include "stages/builtin_stages.h"

#include <rnnoise.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace bomboec {
namespace {

// Сеть обучена на сэмплах в шкале 16 бит.
constexpr float kScale = 32768.0f;

struct StateDeleter {
    void operator()(DenoiseState* st) const { rnnoise_destroy(st); }
};

class RnnoiseStage final : public IStage {
public:
    StageInfo info() const override { return {"rnnoise", fmt_.sampleRate, fmt_.frameSamples, capBit(Cap::Ns), 1}; }

    bool init(const PipelineFormat& fmt, StageParams& cfg, std::string& error) override {
        fmt_ = fmt;
        (void)cfg;  // ключей нет: неизвестные Chain отметит сам
        const auto frame = uint32_t(rnnoise_get_frame_size());
        if (fmt.sampleRate != 48000 || fmt.frameSamples != frame) {
            error = "rnnoise: needs 48 kHz and a " + std::to_string(frame) + "-sample frame";
            return false;
        }
        states_.clear();
        for (uint32_t c = 0; c < fmt.micChannels; ++c) {
            std::unique_ptr<DenoiseState, StateDeleter> st(rnnoise_create(nullptr));
            if (!st) {
                error = "rnnoise: rnnoise_create failed";
                return false;
            }
            states_.push_back(std::move(st));
        }
        in_.assign(frame, 0.0f);
        out_.assign(frame, 0.0f);
        vad_ = 0.0f;
        return true;
    }

    void process(Frame& mic, const Frame*) override {
        const uint32_t n = fmt_.frameSamples;
        for (uint32_t c = 0; c < mic.channels() && c < states_.size(); ++c) {
            float* x = mic.channel(c).data();
            for (uint32_t i = 0; i < n; ++i) in_[i] = x[i] * kScale;
            const float vad = rnnoise_process_frame(states_[c].get(), out_.data(), in_.data());
            if (c == 0) vad_ = vad;
            for (uint32_t i = 0; i < n; ++i) x[i] = out_[i] / kScale;
        }
    }

    void reset() override {
        // Состояние сети сбрасывается повторной инициализацией (аллокация, не для аудиопотока).
        for (auto& st : states_) rnnoise_init(st.get(), nullptr);
        vad_ = 0.0f;
    }

    StageStats stats() const override {
        StageStats s;
        s.vadProbability = vad_;
        return s;
    }

private:
    PipelineFormat fmt_;
    std::vector<std::unique_ptr<DenoiseState, StateDeleter>> states_;
    std::vector<float> in_, out_;
    float vad_ = 0.0f;  // вероятность речи в последнем кадре (канал 0)
};

}  // namespace

std::unique_ptr<IStage> makeRnnoiseStage() { return std::make_unique<RnnoiseStage>(); }

}  // namespace bomboec
