// Стадия шумоподавления на RNNoise (xiph, 0.2): рекуррентная сеть считает усиление по
// 22 полосам на кадр 10 ms при 48 kHz, лучше NS из WebRTC на нестационарных шумах
// (клавиатура, щелчки). Алгоритмическая задержка два кадра, ровно 960 сэмплов: окно анализа
// 20 ms с перекрытием, и усиление, посчитанное по новому кадру, ложится на предыдущий спектр
// (delayed_X в denoise.c, сеть заглядывает на кадр вперёд).
//
// Ключи конфига:
//   input_gain_db = 0   усиление перед сетью с точным обратным делением после неё. Меняет только
//                       рабочую точку сети (она обучена на шкале 16 бит и тихую речь портит
//                       сильнее громкой), уровень сигнала на выходе стадии не меняется.
//   gate = false        гейт ударов и щелчков (transient_gate.h) с ключами gate_* как у стадии
//                       transient. Детектор смотрит на вход сети, усиление ложится на её выход:
//                       задержка сети покрывает lookahead, своей задержки у гейта нет, и фаза
//                       кадров сети та же, что без гейта. По умолчанию (свип корпуса 2026-09-14,
//                       docs/research/2026-09-14-transient-gate.md):
//                         gate_mode = "duck", gate_depth_db = 30, gate_margin_db = 15,
//                         gate_hold_ms = 60, gate_release_ms = 10, gate_harmonicity_max = 0.4,
//                         gate_lookahead_ms = 8 (до 20), gate_rise_db_per_ms = 20,
//                         gate_level_dbfs = -30, gate_preroll_ms = 2
//
// Пробовалась смесь с сухим сигналом (mix) с выравниванием на один кадр: она теряла 2 dB речи.
// Задержка сети два кадра, так что сухой сигнал был сдвинут на 10 ms; с верным сдвигом не проверялось.
//
// Заголовок rnnoise не выходит за пределы этого файла.

#include "stages/builtin_stages.h"
#include "stages/transient_gate.h"

#include <rnnoise.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace bomboec {
namespace {

// Сеть обучена на сэмплах в шкале 16 бит.
constexpr float kScale = 32768.0f;
// Задержка сети в кадрах (см. заголовок файла).
constexpr uint32_t kLatencyFrames = 2;

struct StateDeleter {
    void operator()(DenoiseState* st) const { rnnoise_destroy(st); }
};

class RnnoiseStage final : public IStage {
public:
    StageInfo info() const override {
        const uint32_t caps = capBit(Cap::Ns) | (gateOn_ ? capBit(Cap::Transient) : 0u);
        return {"rnnoise", fmt_.sampleRate, fmt_.frameSamples, caps, kLatencyFrames};
    }

    bool init(const PipelineFormat& fmt, StageParams& cfg, std::string& error) override {
        fmt_ = fmt;
        const double gainDb = cfg.number("input_gain_db", 0.0, -40.0, 40.0);
        gateOn_ = cfg.boolean("gate", false);
        if (!cfg.ok()) {
            error = "rnnoise: " + cfg.error();
            return false;
        }
        gain_ = float(std::pow(10.0, gainDb / 20.0));
        const auto frame = uint32_t(rnnoise_get_frame_size());
        if (fmt.sampleRate != 48000 || fmt.frameSamples != frame) {
            error = "rnnoise: needs 48 kHz and a " + std::to_string(frame) + "-sample frame";
            return false;
        }
        // Ключи гейта читаются и при gate = false: выключенный гейт с настройками - не опечатка.
        TransientGate::Defaults gate;
        gate.mode = "duck";
        gate.depthDb = 30.0;
        gate.marginDb = 15.0;
        gate.releaseMs = 10.0;
        gate.harmonicityMax = 0.4;
        gate.lookaheadMs = 8.0;
        gate.maxLookaheadMs = 20.0;
        gate.outputDelaySamples = kLatencyFrames * fmt.frameSamples;
        if (!gate_.init(fmt, cfg, "gate_", gate, error)) {
            error = "rnnoise: " + error;  // имя ключа уже с gate_
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
        if (gateOn_ && mic.channels() > 0) gate_.analyze(mic.channel(0).data());  // вход сети
        for (uint32_t c = 0; c < mic.channels() && c < states_.size(); ++c) {
            float* x = mic.channel(c).data();
            const float scale = kScale * gain_;
            for (uint32_t i = 0; i < n; ++i) in_[i] = x[i] * scale;
            const float vad = rnnoise_process_frame(states_[c].get(), out_.data(), in_.data());
            if (c == 0) vad_ = vad;
            for (uint32_t i = 0; i < n; ++i) x[i] = out_[i] / scale;
            if (gateOn_ && gate_.minGain() < 0.999f) {
                const std::span<const float> gains = gate_.gains();  // уже сдвинуто на задержку сети
                for (uint32_t i = 0; i < n; ++i) x[i] *= gains[i];
            }
            if (gateOn_ && c == 0) gate_.observeOutput(x);  // фон для duck: то, что слышно
        }
    }

    void reset() override {
        // Состояние сети сбрасывается повторной инициализацией (аллокация, не для аудиопотока).
        for (auto& st : states_) rnnoise_init(st.get(), nullptr);
        gate_.reset();
        vad_ = 0.0f;
    }

    StageStats stats() const override {
        StageStats s;
        s.vadProbability = vad_;
        if (gateOn_) {
            s.transients = gate_.triggers();
            if (gate_.minGain() < 0.999f) s.gainDb = 20.0 * std::log10(std::max(gate_.minGain(), 1e-6f));
        }
        return s;
    }

private:
    PipelineFormat fmt_;
    std::vector<std::unique_ptr<DenoiseState, StateDeleter>> states_;
    std::vector<float> in_, out_;
    float gain_ = 1.0f;  // усиление перед сетью (после неё делится обратно)
    bool gateOn_ = false;
    TransientGate gate_;
    float vad_ = 0.0f;  // вероятность речи в последнем кадре (канал 0)
};

}  // namespace

std::unique_ptr<IStage> makeRnnoiseStage() { return std::make_unique<RnnoiseStage>(); }

}  // namespace bomboec
