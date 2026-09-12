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
// Настройка подавителя AEC3 (все значения по умолчанию равны штатным в EchoCanceller3Config,
// то есть без этих ключей поведение прежнее). Подавитель считает усиление по маскировочным
// порогам: enr (echo-to-nearend) ниже enr_transparent - усиление 1, выше enr_suppress - полное
// подавление; чем больше пороги, тем больше остаётся голоса и эха. Наборов два: normal_* и
// nearend_* (последний действует, когда детектор считает, что говорит человек, а не колонки).
//   nearend_mask_lf_transparent = 1.09, nearend_mask_lf_suppress = 1.1
//   nearend_mask_hf_transparent = 0.1,  nearend_mask_hf_suppress = 0.3
//   normal_mask_lf_transparent = 0.3,   normal_mask_lf_suppress = 0.4
//   normal_mask_hf_transparent = 0.07,  normal_mask_hf_suppress = 0.1
//   suppressor_max_dec_factor_lf = 0.25  как быстро падает усиление НЧ за блок (больше = плавнее)
// Детектор "говорит человек" (dominant nearend): срабатывает, когда эхо/голос ниже
// enr_threshold и snr выше snr_threshold, держится hold_duration блоков, требует
// trigger_threshold подряд.
//   nearend_enr_threshold = 0.25, nearend_snr_threshold = 30
//   nearend_hold_duration = 50,   nearend_trigger_threshold = 12
//   high_bands_max_gain_during_echo = 1.0   потолок усиления выше 8 kHz во время эха
//   conservative_hf_suppression = false
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

// Значения подавителя AEC3 из конфига; по умолчанию совпадают с EchoCanceller3Config.
struct SuppressorTuning {
    double nearendLfTransparent = 1.09, nearendLfSuppress = 1.1;
    double nearendHfTransparent = 0.1, nearendHfSuppress = 0.3;
    double normalLfTransparent = 0.3, normalLfSuppress = 0.4;
    double normalHfTransparent = 0.07, normalHfSuppress = 0.1;
    double maxDecFactorLf = 0.25;
    double nearendEnrThreshold = 0.25, nearendSnrThreshold = 30.0;
    int64_t nearendHoldDuration = 50, nearendTriggerThreshold = 12;
    double highBandsMaxGain = 1.0;
    bool conservativeHf = false;
};

void readSuppressorTuning(StageParams& cfg, SuppressorTuning& t) {
    t.nearendLfTransparent = cfg.number("nearend_mask_lf_transparent", t.nearendLfTransparent, 0.0, 100.0);
    t.nearendLfSuppress = cfg.number("nearend_mask_lf_suppress", t.nearendLfSuppress, 0.0, 100.0);
    t.nearendHfTransparent = cfg.number("nearend_mask_hf_transparent", t.nearendHfTransparent, 0.0, 100.0);
    t.nearendHfSuppress = cfg.number("nearend_mask_hf_suppress", t.nearendHfSuppress, 0.0, 100.0);
    t.normalLfTransparent = cfg.number("normal_mask_lf_transparent", t.normalLfTransparent, 0.0, 100.0);
    t.normalLfSuppress = cfg.number("normal_mask_lf_suppress", t.normalLfSuppress, 0.0, 100.0);
    t.normalHfTransparent = cfg.number("normal_mask_hf_transparent", t.normalHfTransparent, 0.0, 100.0);
    t.normalHfSuppress = cfg.number("normal_mask_hf_suppress", t.normalHfSuppress, 0.0, 100.0);
    t.maxDecFactorLf = cfg.number("suppressor_max_dec_factor_lf", t.maxDecFactorLf, 0.0, 100.0);
    t.nearendEnrThreshold = cfg.number("nearend_enr_threshold", t.nearendEnrThreshold, 0.0, 1000.0);
    t.nearendSnrThreshold = cfg.number("nearend_snr_threshold", t.nearendSnrThreshold, 0.0, 1000.0);
    t.nearendHoldDuration = cfg.integer("nearend_hold_duration", t.nearendHoldDuration, 0, 10000);
    t.nearendTriggerThreshold = cfg.integer("nearend_trigger_threshold", t.nearendTriggerThreshold, 0, 10000);
    t.highBandsMaxGain = cfg.number("high_bands_max_gain_during_echo", t.highBandsMaxGain, 0.0, 1.0);
    t.conservativeHf = cfg.boolean("conservative_hf_suppression", t.conservativeHf);
}

void applySuppressorTuning(const SuppressorTuning& t, webrtc::EchoCanceller3Config& aec3) {
    auto& s = aec3.suppressor;
    s.nearend_tuning.mask_lf.enr_transparent = float(t.nearendLfTransparent);
    s.nearend_tuning.mask_lf.enr_suppress = float(t.nearendLfSuppress);
    s.nearend_tuning.mask_hf.enr_transparent = float(t.nearendHfTransparent);
    s.nearend_tuning.mask_hf.enr_suppress = float(t.nearendHfSuppress);
    s.normal_tuning.mask_lf.enr_transparent = float(t.normalLfTransparent);
    s.normal_tuning.mask_lf.enr_suppress = float(t.normalLfSuppress);
    s.normal_tuning.mask_hf.enr_transparent = float(t.normalHfTransparent);
    s.normal_tuning.mask_hf.enr_suppress = float(t.normalHfSuppress);
    s.nearend_tuning.max_dec_factor_lf = float(t.maxDecFactorLf);
    s.normal_tuning.max_dec_factor_lf = float(t.maxDecFactorLf);
    s.dominant_nearend_detection.enr_threshold = float(t.nearendEnrThreshold);
    s.dominant_nearend_detection.snr_threshold = float(t.nearendSnrThreshold);
    s.dominant_nearend_detection.hold_duration = int(t.nearendHoldDuration);
    s.dominant_nearend_detection.trigger_threshold = int(t.nearendTriggerThreshold);
    s.high_bands_suppression.max_gain_during_echo = float(t.highBandsMaxGain);
    s.conservative_hf_suppression = t.conservativeHf;
}

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
        readSuppressorTuning(cfg, s.tuning);
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
        SuppressorTuning tuning;
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
            applySuppressorTuning(s.tuning, aec3);
            if (!webrtc::EchoCanceller3Config::Validate(&aec3)) {
                error = "webrtc: EchoCanceller3Config rejected (filter_length_blocks/delay_num_filters/suppressor)";
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
