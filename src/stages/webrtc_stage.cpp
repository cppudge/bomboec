// Стадия на WebRTC AudioProcessing: AEC3 плюс, по конфигу, HPF/NS/AGC из того же APM.
//
// Ключи конфига:
//   aec = true                    AEC3
//   hpf = false                   high-pass filter APM
//   ns = false, ns_level = "moderate"   low | moderate | high | very_high
//   agc = false                   gain_controller2 (адаптивный цифровой)
//   filter_length_blocks = 13     длина адаптивного фильтра AEC3 (блоки по 4 ms)
//   delay_num_filters = 5         число фильтров оценки задержки (диапазон поиска)
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

    bool init(const PipelineFormat& fmt, const toml::table& cfg, std::string& error) override {
        fmt_ = fmt;
        if (&cfg != &lastCfg_) lastCfg_ = cfg;
        if (fmt.frameSamples * 100 != fmt.sampleRate) {
            error = "webrtc: frame must be exactly 10 ms";
            return false;
        }

        const bool aec = cfg["aec"].value_or(true);
        const bool hpf = cfg["hpf"].value_or(false);
        const bool ns = cfg["ns"].value_or(false);
        const bool agc = cfg["agc"].value_or(false);
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
            const int64_t filterBlocks = cfg["filter_length_blocks"].value_or(int64_t(13));
            const int64_t delayFilters = cfg["delay_num_filters"].value_or(int64_t(5));
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
        const std::string level = cfg["ns_level"].value_or(std::string("moderate"));
        using NS = webrtc::AudioProcessing::Config::NoiseSuppression;
        if (level == "low") c.noise_suppression.level = NS::kLow;
        else if (level == "moderate") c.noise_suppression.level = NS::kModerate;
        else if (level == "high") c.noise_suppression.level = NS::kHigh;
        else if (level == "very_high") c.noise_suppression.level = NS::kVeryHigh;
        else {
            error = "webrtc: ns_level must be low|moderate|high|very_high";
            return false;
        }
        c.gain_controller1.enabled = false;
        c.gain_controller2.enabled = agc;
        c.gain_controller2.adaptive_digital.enabled = agc;
        apm_->ApplyConfig(c);

        micCfg_ = webrtc::StreamConfig(int(fmt.sampleRate), size_t(fmt.micChannels));
        refCfg_ = webrtc::StreamConfig(int(fmt.sampleRate), size_t(fmt.referenceChannels));
        refScratch_.resize(fmt.referenceChannels, fmt.frameSamples);
        silence_.resize(fmt.referenceChannels, fmt.frameSamples);
        return true;
    }

    void process(Frame& mic, const Frame* reference) override {
        if (hasCap(caps_, Cap::Aec)) {
            // Reverse stream нужен каждый кадр, иначе AEC3 теряет таймлайн render.
            const Frame& ref = reference ? *reference : silence_;
            apm_->ProcessReverseStream(ref.planes(), refCfg_, refCfg_, refScratch_.planes());
        }
        apm_->ProcessStream(mic.planes(), micCfg_, micCfg_, mic.planes());
    }

    void reset() override {
        // У APM нет публичного reset состояния AEC; пересоздаём с той же конфигурацией.
        if (!apm_) return;
        std::string error;
        init(fmt_, lastCfg_, error);
    }

    StageStats stats() const override {
        StageStats s;
        if (!apm_) return s;
        const webrtc::AudioProcessingStats st = apm_->GetStatistics();
        if (st.delay_ms) s.delayMs = double(*st.delay_ms);
        if (st.echo_return_loss) s.erlDb = *st.echo_return_loss;
        if (st.echo_return_loss_enhancement) s.erleDb = *st.echo_return_loss_enhancement;
        if (st.residual_echo_likelihood) s.residualEchoLikelihood = *st.residual_echo_likelihood;
        return s;
    }

private:
    PipelineFormat fmt_;
    uint32_t caps_ = 0;
    toml::table lastCfg_;
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
    webrtc::StreamConfig micCfg_;
    webrtc::StreamConfig refCfg_;
    Frame refScratch_;
    Frame silence_;
};

}  // namespace

std::unique_ptr<IStage> makeWebrtcStage() { return std::make_unique<WebrtcStage>(); }

}  // namespace bomboec
