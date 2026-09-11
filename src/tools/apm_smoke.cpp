// Этап 0: проверка, что WebRTC APM с AEC3 собирается, линкуется и работает.
// Прогоняет синтетический сигнал: reference = шум, mic = задержанная копия reference
// плюс слабый шум, и печатает статистику AEC (ERL/ERLE/delay).

#include <api/audio/audio_processing.h>
#include <api/audio/echo_canceller3_config.h>
#include <api/audio/echo_canceller3_factory.h>
#include <api/scoped_refptr.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <random>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr int kFrame = kRate / 100;  // 10 ms
constexpr int kRenderChannels = 2;
constexpr int kEchoDelaySamples = 2400;  // 50 ms
constexpr int kSeconds = 5;

int run() {
    // Явный конфиг AEC3 через фабрику: так приложение сможет менять длину
    // фильтра, диапазон поиска задержки и т.п. без внутренних заголовков.
    webrtc::EchoCanceller3Config aec3;
    aec3.filter.refined.length_blocks = 20;
    aec3.filter.coarse.length_blocks = 20;
    if (!webrtc::EchoCanceller3Config::Validate(&aec3)) {
        std::fputs("EchoCanceller3Config was adjusted during validation", stderr);
        std::fputc('\n', stderr);
    }
    const webrtc::scoped_refptr<webrtc::AudioProcessing> apm =
        webrtc::AudioProcessingBuilder()
            .SetEchoControlFactory(std::make_unique<webrtc::EchoCanceller3Factory>(aec3))
            .Create();
    if (!apm) {
        std::fprintf(stderr, "AudioProcessingBuilder().Create() failed\n");
        return 1;
    }

    webrtc::AudioProcessing::Config cfg;
    cfg.echo_canceller.enabled = true;
    cfg.echo_canceller.mobile_mode = false;
    cfg.high_pass_filter.enabled = true;
    cfg.noise_suppression.enabled = false;
    cfg.gain_controller1.enabled = false;
    cfg.gain_controller2.enabled = false;
    apm->ApplyConfig(cfg);

    const webrtc::StreamConfig renderCfg(kRate, kRenderChannels);
    const webrtc::StreamConfig captureCfg(kRate, 1);

    std::mt19937 rng(42);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    std::normal_distribution<float> micNoise(0.0f, 0.001f);

    // История reference для эмуляции акустической задержки.
    std::vector<float> history(kEchoDelaySamples, 0.0f);
    size_t histPos = 0;

    std::array<float, kFrame> renderL{}, renderR{}, mic{}, out{};
    float* renderPlanes[kRenderChannels] = {renderL.data(), renderR.data()};
    float* micPlanes[1] = {mic.data()};
    float* outPlanes[1] = {out.data()};

    double inEnergy = 0.0, outEnergy = 0.0;
    const int totalFrames = kSeconds * 100;

    for (int f = 0; f < totalFrames; ++f) {
        for (int i = 0; i < kFrame; ++i) {
            const float s = noise(rng);
            renderL[i] = s;
            renderR[i] = s * 0.8f;
            // Эхо: половина L и R, задержанные на kEchoDelaySamples.
            const float echo = 0.5f * history[histPos];
            history[histPos] = 0.5f * (renderL[i] + renderR[i]);
            histPos = (histPos + 1) % kEchoDelaySamples;
            mic[i] = echo + micNoise(rng);
        }

        int err = apm->ProcessReverseStream(renderPlanes, renderCfg, renderCfg, renderPlanes);
        if (err != webrtc::AudioProcessing::kNoError) {
            std::fprintf(stderr, "ProcessReverseStream error %d\n", err);
            return 2;
        }
        err = apm->ProcessStream(micPlanes, captureCfg, captureCfg, outPlanes);
        if (err != webrtc::AudioProcessing::kNoError) {
            std::fprintf(stderr, "ProcessStream error %d\n", err);
            return 3;
        }

        // Считаем энергию только на последней секунде, когда фильтр сошёлся.
        if (f >= totalFrames - 100) {
            for (int i = 0; i < kFrame; ++i) {
                inEnergy += double(mic[i]) * mic[i];
                outEnergy += double(out[i]) * out[i];
            }
        }
    }

    const webrtc::AudioProcessingStats st = apm->GetStatistics();
    auto opt = [](const auto& v) { return v.has_value() ? static_cast<double>(*v) : std::nan(""); };
    const double attenuationDb = 10.0 * std::log10(inEnergy / (outEnergy + 1e-12));

    std::printf("webrtc-audio-processing smoke test\n");
    std::printf("  rate=%d frame=%d render_ch=%d echo_delay_ms=%d\n", kRate, kFrame, kRenderChannels,
                kEchoDelaySamples * 1000 / kRate);
    std::printf("  echo attenuation (last 1 s): %.1f dB\n", attenuationDb);
    std::printf("  stats: delay_ms=%.0f erl=%.1f erle=%.1f residual_echo_likelihood=%.2f\n", opt(st.delay_ms),
                opt(st.echo_return_loss), opt(st.echo_return_loss_enhancement), opt(st.residual_echo_likelihood));

    return attenuationDb > 15.0 ? 0 : 4;
}

}  // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
