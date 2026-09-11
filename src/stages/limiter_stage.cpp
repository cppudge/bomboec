// Простой пиковый лимитер без lookahead: мгновенная атака, экспоненциальный
// release. Гарантирует |y| <= ceiling. Последняя стадия перед виртуальным микрофоном.

#include "stages/builtin_stages.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace bomboec {
namespace {

class LimiterStage final : public IStage {
public:
    StageInfo info() const override { return {"limiter", fmt_.sampleRate, fmt_.frameSamples, capBit(Cap::Limiter), 0}; }

    bool init(const PipelineFormat& fmt, StageParams& cfg, std::string& error) override {
        fmt_ = fmt;
        const double ceilingDb = cfg.number("ceiling_db", -1.0, -60.0, 0.0);
        const double releaseMs = cfg.number("release_ms", 50.0, 0.1, 10000.0);
        if (!cfg.ok()) {
            error = "limiter: " + cfg.error();
            return false;
        }
        ceiling_ = float(std::pow(10.0, ceilingDb / 20.0));
        releaseCoef_ = float(1.0 - std::exp(-1.0 / (releaseMs / 1000.0 * fmt.sampleRate)));
        gains_.assign(fmt.micChannels, 1.0f);
        minGain_ = 1.0f;
        return true;
    }

    void process(Frame& mic, const Frame*) override {
        float minGain = 1.0f;
        for (uint32_t c = 0; c < mic.channels() && c < gains_.size(); ++c) {
            float g = gains_[c];
            for (float& x : mic.channel(c)) {
                // Сначала release, затем жёсткое ограничение: так |x*g| <= ceiling всегда.
                g += (1.0f - g) * releaseCoef_;
                const float peak = std::fabs(x);
                if (peak * g > ceiling_) {
                    g = ceiling_ / peak;
                }
                x *= g;
                minGain = std::min(minGain, g);
            }
            gains_[c] = g;
        }
        minGain_ = minGain;
    }

    void reset() override {
        std::fill(gains_.begin(), gains_.end(), 1.0f);
        minGain_ = 1.0f;
    }

    StageStats stats() const override {
        StageStats s;
        s.gainDb = 20.0 * std::log10(std::max(minGain_, 1e-6f));
        return s;
    }

private:
    PipelineFormat fmt_;
    float ceiling_ = 1.0f;
    float releaseCoef_ = 0.0f;
    std::vector<float> gains_;
    float minGain_ = 1.0f;
};

}  // namespace

std::unique_ptr<IStage> makeLimiterStage() { return std::make_unique<LimiterStage>(); }

}  // namespace bomboec
