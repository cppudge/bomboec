// Фильтр высоких частот: биквад Баттерворта 2-го порядка на каждый канал mic.
// Убирает DC и низкочастотный гул до AEC/NS.

#include "stages/builtin_stages.h"

#include <cmath>
#include <numbers>
#include <vector>

namespace bomboec {
namespace {

class HpfStage final : public IStage {
public:
    StageInfo info() const override { return {"hpf", fmt_.sampleRate, fmt_.frameSamples, capBit(Cap::Hpf), 0}; }

    bool init(const PipelineFormat& fmt, const toml::table& cfg, std::string& error) override {
        fmt_ = fmt;
        const double cutoff = cfg["cutoff_hz"].value_or(80.0);
        if (cutoff <= 0.0 || cutoff >= fmt.sampleRate / 2.0) {
            error = "hpf: cutoff_hz must be in (0, sample_rate/2)";
            return false;
        }
        const double w0 = 2.0 * std::numbers::pi * cutoff / fmt.sampleRate;
        const double cosw0 = std::cos(w0);
        const double alpha = std::sin(w0) * std::numbers::sqrt2 / 2.0;  // sin(w0) / (2Q), Q = 1/sqrt(2)
        const double a0 = 1.0 + alpha;
        b0_ = float((1.0 + cosw0) / 2.0 / a0);
        b1_ = float(-(1.0 + cosw0) / a0);
        b2_ = b0_;
        a1_ = float(-2.0 * cosw0 / a0);
        a2_ = float((1.0 - alpha) / a0);
        states_.assign(fmt.micChannels, {});
        return true;
    }

    void process(Frame& mic, const Frame*) override {
        for (uint32_t c = 0; c < mic.channels() && c < states_.size(); ++c) {
            State& s = states_[c];
            for (float& x : mic.channel(c)) {
                // Direct form II transposed.
                const float y = b0_ * x + s.z1;
                s.z1 = b1_ * x - a1_ * y + s.z2;
                s.z2 = b2_ * x - a2_ * y;
                x = y;
            }
        }
    }

    void reset() override {
        for (State& s : states_) s = {};
    }

    StageStats stats() const override { return {}; }

private:
    struct State {
        float z1 = 0.0f;
        float z2 = 0.0f;
    };
    PipelineFormat fmt_;
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    std::vector<State> states_;
};

}  // namespace

std::unique_ptr<IStage> makeHpfStage() { return std::make_unique<HpfStage>(); }

}  // namespace bomboec
