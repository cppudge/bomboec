// Стадия на WebRTC AudioProcessing: AEC3 плюс, по конфигу, HPF/NS/AGC из того же APM.
//
// Ключи конфига:
//   aec = true                    AEC3
//   hpf = false                   high-pass filter APM
//   ns = false, ns_level = "moderate"   low | moderate | high | very_high
//   agc = false                   gain_controller2 (адаптивный цифровой)
//   filter_length_blocks = 13     длина адаптивного фильтра AEC3 (блоки по 4 ms), 1..60
//   delay_num_filters = 5         число фильтров оценки задержки (диапазон поиска), 1..20
//
// Заголовки WebRTC не выходят за пределы этого файла.

#include "stages/builtin_stages.h"

#include <api/audio/audio_processing.h>
#include <api/audio/echo_canceller3_config.h>
#include <api/audio/echo_canceller3_factory.h>
#include <api/scoped_refptr.h>

#include <algorithm>
#include <memory>
#include <string>

namespace bomboec {
namespace {

class WebrtcStage final : public IStage {
public:
    StageInfo info() const override { return {"webrtc", fmt_.sampleRate, fmt_.frameSamples, caps_, 0}; }

    bool init(const PipelineFormat& fmt, StageParams& cfg, std::string& error) override {
        fmt_ = fmt;
        if (fmt.frameSamples * 100 != fmt.sampleRate) {
            error = "webrtc: frame must be exactly 10 ms";
            return false;
        }

        Settings s;
        s.aec = cfg.boolean("aec", true);
        s.hpf = cfg.boolean("hpf", false);
        s.ns = cfg.boolean("ns", false);
        s.agc = cfg.boolean("agc", false);
        s.filterBlocks = cfg.integer("filter_length_blocks", 13, 1, 60);
        s.delayFilters = cfg.integer("delay_num_filters", 5, 1, 20);
        s.nsLevel = cfg.choice("ns_level", "moderate", {"low", "moderate", "high", "very_high"});
        if (!cfg.ok()) {
            error = "webrtc: " + cfg.error();
            return false;
        }
        return apply(s, error);
    }

    void process(Frame& mic, const Frame* reference) override {
        if (hasCap(caps_, Cap::Aec)) {
            // Reverse stream нужен каждый кадр, иначе AEC3 теряет таймлайн render.
            const Frame& ref = reference ? *reference : silence_;
            if (apm_->ProcessReverseStream(ref.planes(), refCfg_, refCfg_, refScratch_.planes()) !=
                webrtc::AudioProcessing::kNoError) {
                ++errors_;
            }
        }
        if (apm_->ProcessStream(mic.planes(), micCfg_, micCfg_, mic.planes()) != webrtc::AudioProcessing::kNoError) {
            ++errors_;
        }
    }

    void reset() override {
        // У APM нет публичного reset состояния AEC; пересоздаём с той же конфигурацией.
        if (!apm_) return;
        std::string error;
        apply(settings_, error);
    }

    StageStats stats() const override {
        StageStats s;
        if (!apm_) return s;
        s.errors = errors_;
        const webrtc::AudioProcessingStats st = apm_->GetStatistics();
        if (st.delay_ms) s.delayMs = double(*st.delay_ms);
        if (st.echo_return_loss) s.erlDb = *st.echo_return_loss;
        if (st.echo_return_loss_enhancement) s.erleDb = *st.echo_return_loss_enhancement;
        if (st.residual_echo_likelihood) s.residualEchoLikelihood = *st.residual_echo_likelihood;
        return s;
    }

private:
    struct Settings {
        bool aec = true, hpf = false, ns = false, agc = false;
        int64_t filterBlocks = 13, delayFilters = 5;
        std::string nsLevel = "moderate";
    };

    bool apply(const Settings& s, std::string& error) {
        settings_ = s;
        const bool aec = s.aec, hpf = s.hpf, ns = s.ns, agc = s.agc;
        caps_ = 0;
        if (aec) caps_ |= capBit(Cap::Aec);
        if (hpf) caps_ |= capBit(Cap::Hpf);
        if (ns) caps_ |= capBit(Cap::Ns);
        if (agc) caps_ |= capBit(Cap::Agc);
        if (caps_ == 0) {
            error = "webrtc: nothing enabled (aec/hpf/ns/agc all false)";
            return false;
        }

        webrtc::AudioProcessingBuilder builder;
        if (aec) {
            webrtc::EchoCanceller3Config aec3;
            const int64_t filterBlocks = s.filterBlocks;
            const int64_t delayFilters = s.delayFilters;
            // Reference всегда подаётся многоканальным. С детектором стерео AEC3 при смене
            // характера контента (моно-голос <-> стереомузыка) пересоздавал бы BlockProcessor
            // прямо в mic-потоке (аллокация) и сбрасывал адаптивный фильтр.
            aec3.multi_channel.detect_stereo_content = false;
            aec3.filter.refined.length_blocks = size_t(filterBlocks);
            aec3.filter.coarse.length_blocks = size_t(filterBlocks);
            aec3.filter.refined_initial.length_blocks =
                std::min(aec3.filter.refined_initial.length_blocks, size_t(filterBlocks));
            aec3.filter.coarse_initial.length_blocks =
                std::min(aec3.filter.coarse_initial.length_blocks, size_t(filterBlocks));
            aec3.delay.num_filters = size_t(delayFilters);
            if (!webrtc::EchoCanceller3Config::Validate(&aec3)) {
                error = "webrtc: EchoCanceller3Config rejected (filter_length_blocks/delay_num_filters)";
                return false;
            }
            builder.SetEchoControlFactory(std::make_unique<webrtc::EchoCanceller3Factory>(aec3));
        }
        apm_ = builder.Create();
        if (!apm_) {
            error = "webrtc: AudioProcessingBuilder::Create failed";
            return false;
        }

        webrtc::AudioProcessing::Config c;
        c.echo_canceller.enabled = aec;
        c.echo_canceller.mobile_mode = false;
        c.high_pass_filter.enabled = hpf;
        c.noise_suppression.enabled = ns;
        using NS = webrtc::AudioProcessing::Config::NoiseSuppression;
        c.noise_suppression.level = s.nsLevel == "low"         ? NS::kLow
                                    : s.nsLevel == "high"      ? NS::kHigh
                                    : s.nsLevel == "very_high" ? NS::kVeryHigh
                                                               : NS::kModerate;
        c.gain_controller1.enabled = false;
        c.gain_controller2.enabled = agc;
        c.gain_controller2.adaptive_digital.enabled = agc;
        apm_->ApplyConfig(c);

        micCfg_ = webrtc::StreamConfig(int(fmt_.sampleRate), size_t(fmt_.micChannels));
        refCfg_ = webrtc::StreamConfig(int(fmt_.sampleRate), size_t(fmt_.referenceChannels));
        refScratch_.resize(fmt_.referenceChannels, fmt_.frameSamples);
        silence_.resize(fmt_.referenceChannels, fmt_.frameSamples);
        errors_ = 0;
        return true;
    }

    PipelineFormat fmt_;
    uint32_t caps_ = 0;
    Settings settings_;
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
    webrtc::StreamConfig micCfg_;
    webrtc::StreamConfig refCfg_;
    Frame refScratch_;
    Frame silence_;
    uint64_t errors_ = 0;  // вызовов APM, вернувших ошибку
};

}  // namespace

std::unique_ptr<IStage> makeWebrtcStage() { return std::make_unique<WebrtcStage>(); }

}  // namespace bomboec
