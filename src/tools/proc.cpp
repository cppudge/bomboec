// bomboec-proc: офлайн-прогон записанной пары WAV через цепочку стадий.
//
//   bomboec-proc --mic take/mic.wav --ref take/ref.wav --config config/default.toml --out take/out.wav
//                [--ref-offset-ms 0] [--csv take/stats.csv] [--ref-threshold-db -50]
//
// Печатает статистику раз в секунду и итоговые метрики:
//   - подавление на кадрах, где reference активен (эхо + возможная речь);
//   - подавление на кадрах без reference (ожидается ~0 dB: мера искажения речи);
//   - время сходимости: первая секунда с подавлением >= 10 dB при активном reference.
// --ref-offset-ms > 0 задерживает reference относительно mic (проверка статического сдвига).

#include "core/chain.h"
#include "core/config.h"
#include "core/utf8.h"
#include "core/wav.h"
#include "stages/builtin_stages.h"
#include "tools/console.h"

#include <cxxopts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace bomboec;

namespace {

double dbfs(double rms) { return 20.0 * std::log10(std::max(rms, 1e-9)); }

double frameRms(const Frame& f, uint32_t ch) {
    double e = 0.0;
    for (const float v : f.channel(ch)) e += double(v) * v;
    return std::sqrt(e / f.samples());
}

std::string fmtOpt(const std::optional<double>& v, const char* fmt) {
    if (!v) return "  n/a";
    char buf[32];
    std::snprintf(buf, sizeof(buf), fmt, *v);
    return buf;
}

// Приводит interleaved-данные к нужному числу каналов: 1 <- среднее, N <- первые N / дублирование.
std::vector<float> mapChannels(const std::vector<float>& in, uint32_t inCh, uint32_t outCh) {
    if (inCh == outCh) return in;
    const size_t frames = in.size() / inCh;
    std::vector<float> out(frames * outCh);
    for (size_t i = 0; i < frames; ++i) {
        if (outCh == 1) {
            float s = 0.0f;
            for (uint32_t c = 0; c < inCh; ++c) s += in[i * inCh + c];
            out[i] = s / float(inCh);
        } else {
            for (uint32_t c = 0; c < outCh; ++c) out[i * outCh + c] = in[i * inCh + std::min(c, inCh - 1)];
        }
    }
    return out;
}

int run(int argc, char** argv) {
    cxxopts::Options opts("bomboec-proc", "Offline AEC/NS chain runner");
    // clang-format off
    opts.add_options()
        ("mic", "Microphone WAV", cxxopts::value<std::string>())
        ("ref", "Reference (loopback) WAV", cxxopts::value<std::string>())
        ("config", "Pipeline TOML", cxxopts::value<std::string>()->default_value("config/default.toml"))
        ("out", "Output WAV (mono)", cxxopts::value<std::string>())
        ("ref-offset-ms", "Delay reference relative to mic, ms (negative = advance)", cxxopts::value<double>()->default_value("0"))
        ("ref-threshold-db", "Reference activity threshold, dBFS", cxxopts::value<double>()->default_value("-50"))
        ("csv", "Per-frame stats CSV", cxxopts::value<std::string>()->default_value(""))
        ("quiet", "No per-second progress")
        ("h,help", "Help");
    // clang-format on
    auto args = opts.parse(argc, argv);
    if (args.contains("help") || !args.contains("mic") || !args.contains("ref") || !args.contains("out")) {
        std::printf("%s\n", opts.help().c_str());
        return args.contains("help") ? 0 : 1;
    }

    std::string error;
    AppConfig cfg;
    if (!loadConfig(pathFromUtf8(args["config"].as<std::string>()), cfg, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    const PipelineFormat& fmt = cfg.format;

    std::vector<float> micData, refData;
    uint32_t micCh = 0, micRate = 0, refCh = 0, refRate = 0;
    if (!readWavFile(pathFromUtf8(args["mic"].as<std::string>()), micData, micCh, micRate, error) ||
        !readWavFile(pathFromUtf8(args["ref"].as<std::string>()), refData, refCh, refRate, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 3;
    }
    if (micRate != fmt.sampleRate || refRate != fmt.sampleRate) {
        std::fprintf(stderr, "sample rate mismatch: mic %u, ref %u, pipeline %u (resampling not supported here)\n",
                     micRate, refRate, fmt.sampleRate);
        return 3;
    }
    micData = mapChannels(micData, micCh, fmt.micChannels);
    refData = mapChannels(refData, refCh, fmt.referenceChannels);
    const size_t micFrames = micData.size() / fmt.micChannels;
    const size_t refFrames = refData.size() / fmt.referenceChannels;

    StageRegistry registry;
    registerBuiltinStages(registry);
    const std::unique_ptr<Chain> chain = buildChain(registry, cfg, error);
    if (!chain || !chain->init(fmt, error)) {
        std::fprintf(stderr, "chain: %s\n", error.c_str());
        return 4;
    }

    WavWriter writer;
    if (!writer.open(pathFromUtf8(args["out"].as<std::string>()), fmt.micChannels, fmt.sampleRate, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 5;
    }
    std::FILE* csv = nullptr;
    if (!args["csv"].as<std::string>().empty()) {
        _wfopen_s(&csv, pathFromUtf8(args["csv"].as<std::string>()).wstring().c_str(), L"wb");
        if (csv) std::fprintf(csv, "t_s,mic_dbfs,ref_dbfs,out_dbfs,delay_ms,erl_db,erle_db,residual_echo\n");
    }

    const double offsetMs = args["ref-offset-ms"].as<double>();
    const int64_t offsetSamples = int64_t(std::llround(offsetMs / 1000.0 * fmt.sampleRate));
    const double refThresholdDb = args["ref-threshold-db"].as<double>();
    const bool quiet = args.contains("quiet");

    std::printf("mic %zu frames, ref %zu frames, chain of %zu stages, caps 0x%X, ref offset %+.1f ms\n", micFrames,
                refFrames, chain->size(), chain->caps(), offsetMs);

    Frame mic(fmt.micChannels, fmt.frameSamples);
    Frame ref(fmt.referenceChannels, fmt.frameSamples);
    std::vector<float> outBuf(size_t(fmt.frameSamples) * fmt.micChannels);

    double activeIn = 0.0, activeOut = 0.0, idleIn = 0.0, idleOut = 0.0;
    size_t activeFrames = 0, idleFrames = 0;
    std::optional<double> convergedAt;
    double secIn = 0.0, secRef = 0.0, secOut = 0.0;
    size_t secFrames = 0;

    const size_t totalFrames = micFrames / fmt.frameSamples;
    for (size_t f = 0; f < totalFrames; ++f) {
        const int64_t micStart = int64_t(f) * fmt.frameSamples;
        for (uint32_t c = 0; c < fmt.micChannels; ++c) {
            for (uint32_t i = 0; i < fmt.frameSamples; ++i) {
                mic.channel(c)[i] = micData[size_t(micStart + i) * fmt.micChannels + c];
            }
        }
        const int64_t refStart = micStart - offsetSamples;
        for (uint32_t c = 0; c < fmt.referenceChannels; ++c) {
            for (uint32_t i = 0; i < fmt.frameSamples; ++i) {
                const int64_t idx = refStart + i;
                ref.channel(c)[i] =
                    (idx >= 0 && size_t(idx) < refFrames) ? refData[size_t(idx) * fmt.referenceChannels + c] : 0.0f;
            }
        }

        const double inRms = frameRms(mic, 0);
        double refRms = 0.0;
        for (uint32_t c = 0; c < fmt.referenceChannels; ++c) refRms = std::max(refRms, frameRms(ref, c));

        chain->process(mic, &ref);

        const double outRms = frameRms(mic, 0);
        mic.toInterleaved(outBuf.data());
        writer.write(outBuf.data(), fmt.frameSamples);

        const bool refActive = dbfs(refRms) > refThresholdDb;
        if (refActive) {
            activeIn += inRms * inRms;
            activeOut += outRms * outRms;
            ++activeFrames;
        } else {
            idleIn += inRms * inRms;
            idleOut += outRms * outRms;
            ++idleFrames;
        }

        const StageStats st = chain->stats();
        const double t = double(f) * fmt.frameSamples / fmt.sampleRate;
        secIn += inRms * inRms;
        secRef += refRms * refRms;
        secOut += outRms * outRms;
        ++secFrames;
        // Сходимость: полное подавление (линейный фильтр + подавитель остатка)
        // за последнюю секунду >= 10 dB при активном reference. ERLE из APM
        // отражает только линейную часть и здесь не показателен.
        if (!convergedAt && refActive && secFrames * fmt.frameSamples >= fmt.sampleRate &&
            10.0 * std::log10(secIn / std::max(secOut, 1e-18)) >= 10.0) {
            convergedAt = t;
        }
        if (csv) {
            std::fprintf(csv, "%.3f,%.2f,%.2f,%.2f,%s,%s,%s,%s\n", t, dbfs(inRms), dbfs(refRms), dbfs(outRms),
                         fmtOpt(st.delayMs, "%.0f").c_str(), fmtOpt(st.erlDb, "%.2f").c_str(),
                         fmtOpt(st.erleDb, "%.2f").c_str(), fmtOpt(st.residualEchoLikelihood, "%.3f").c_str());
        }

        if (secFrames * fmt.frameSamples >= fmt.sampleRate) {
            if (quiet) {
                secIn = secRef = secOut = 0.0;
                secFrames = 0;
                continue;
            }
            std::printf(
                "%6.1f s  mic %6.1f  ref %6.1f  out %6.1f dBFS  att %5.1f dB  delay %s ms  erl %s  erle %s  res %s\n",
                t, dbfs(std::sqrt(secIn / secFrames)), dbfs(std::sqrt(secRef / secFrames)),
                dbfs(std::sqrt(secOut / secFrames)), 10.0 * std::log10(secIn / std::max(secOut, 1e-18)),
                fmtOpt(st.delayMs, "%3.0f").c_str(), fmtOpt(st.erlDb, "%5.1f").c_str(),
                fmtOpt(st.erleDb, "%5.1f").c_str(), fmtOpt(st.residualEchoLikelihood, "%4.2f").c_str());
            secIn = secRef = secOut = 0.0;
            secFrames = 0;
        }
    }
    writer.close();
    if (csv) std::fclose(csv);

    std::printf("\nsummary\n");
    std::printf("  frames: %zu total, %zu with reference active (> %.0f dBFS), %zu idle\n", totalFrames, activeFrames,
                refThresholdDb, idleFrames);
    if (activeFrames) {
        std::printf("  attenuation with reference active: %.1f dB (mic %.1f -> out %.1f dBFS)\n",
                    10.0 * std::log10(activeIn / std::max(activeOut, 1e-18)), dbfs(std::sqrt(activeIn / activeFrames)),
                    dbfs(std::sqrt(activeOut / activeFrames)));
    }
    if (idleFrames) {
        std::printf("  attenuation with reference idle:   %.1f dB (mic %.1f -> out %.1f dBFS)\n",
                    10.0 * std::log10(idleIn / std::max(idleOut, 1e-18)), dbfs(std::sqrt(idleIn / idleFrames)),
                    dbfs(std::sqrt(idleOut / idleFrames)));
    }
    if (convergedAt) {
        std::printf("  attenuation >= 10 dB over 1 s first reached at %.2f s\n", *convergedAt);
    } else {
        std::printf("  attenuation never reached 10 dB over a full second with reference active\n");
    }
    const StageStats st = chain->stats();
    std::printf("  final stats: delay %s ms, erl %s dB, erle %s dB\n", fmtOpt(st.delayMs, "%.0f").c_str(),
                fmtOpt(st.erlDb, "%.1f").c_str(), fmtOpt(st.erleDb, "%.1f").c_str());
    std::printf("  output: %s\n", args["out"].as<std::string>().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bomboec::tools::useUtf8Console();
    // Исключения (разбор аргументов cxxopts, toml, std): сообщение вместо abort.
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
