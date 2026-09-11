// bomboec-play: воспроизведение тестового сигнала или WAV в render endpoint.
//
//   bomboec-play --output <id> --seconds 10 [--wav file.wav | --noise] [--gain-db -12]
//
// Без --wav играет чирп 100..8000 Hz (1 с) с паузой 0.5 с; --noise: полосовой шум 100-6000 Hz
// (широкополосный источник эха для записи корпуса).
//
// Используется для проверки virtual cable и калибровки задержки.

#include "core/utf8.h"
#include "core/wav.h"
#include "tools/console.h"
#include "wasapi/com_util.h"
#include "wasapi/devices.h"
#include "wasapi/render_stream.h"

#include <cxxopts.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <numbers>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace bomboec;
namespace ws = bomboec::wasapi;

namespace {

int run(int argc, char** argv) {
    cxxopts::Options opts("bomboec-play", "Play a chirp or WAV into a render endpoint");
    // clang-format off
    opts.add_options()
        ("output", "Render endpoint id (default: system default)", cxxopts::value<std::string>()->default_value(""))
        ("seconds", "Duration", cxxopts::value<double>()->default_value("10"))
        ("wav", "WAV file to play (looped); otherwise a 100..8000 Hz chirp", cxxopts::value<std::string>()->default_value(""))
        ("noise", "Band-limited noise 100..6000 Hz instead of the chirp")
        ("gain-db", "Gain in dB", cxxopts::value<double>()->default_value("-12"))
        ("h,help", "Help");
    // clang-format on
    auto args = opts.parse(argc, argv);
    if (args.contains("help")) {
        std::printf("%s\n", opts.help().c_str());
        return 0;
    }

    const ws::ComInit com;
    std::string error;
    const ws::ComPtr<IMMDevice> dev =
        ws::openDevice(ws::Flow::Render, ws::fromUtf8(args["output"].as<std::string>()), error);
    if (!dev) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    const uint32_t rate = 48000;
    const float gain = float(std::pow(10.0, args["gain-db"].as<double>() / 20.0));

    // Источник: WAV (в моно, зациклен) или чирп 1 с с паузой 0.5 с.
    std::vector<float> source;
    if (!args["wav"].as<std::string>().empty()) {
        std::vector<float> data;
        uint32_t ch = 0, r = 0;
        if (!readWavFile(pathFromUtf8(args["wav"].as<std::string>()), data, ch, r, error) || r != rate) {
            std::fprintf(stderr, "%s (need 48 kHz)\n", error.c_str());
            return 3;
        }
        source.resize(data.size() / ch);
        for (size_t i = 0; i < source.size(); ++i) {
            float s = 0.0f;
            for (uint32_t c = 0; c < ch; ++c) s += data[i * ch + c];
            source[i] = s / float(ch);
        }
    } else if (args.contains("noise")) {
        // Белый шум через два биквада: ФВЧ 100 Hz и ФНЧ 6 kHz (RBJ, Q 0.707), 10 с в цикле.
        std::mt19937 rng(11);
        std::normal_distribution<float> noise(0.0f, 0.15f);
        source.resize(size_t(rate) * 10);
        for (float& x : source) x = noise(rng);
        for (const auto [hz, highpass] : {std::pair{100.0, true}, std::pair{6000.0, false}}) {
            const double w0 = 2.0 * std::numbers::pi * hz / rate, alpha = std::sin(w0) / (2.0 * 0.7071);
            const double cw = std::cos(w0), a0 = 1.0 + alpha;
            const double b0 = (highpass ? (1.0 + cw) : (1.0 - cw)) / 2.0 / a0;
            const double b1 = (highpass ? -(1.0 + cw) : (1.0 - cw)) / a0;
            const double a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
            double z1 = 0.0, z2 = 0.0;
            for (float& x : source) {
                const double y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b0 * x - a2 * y;
                x = float(y);
            }
        }
        float peak = 1e-6f;
        for (const float x : source) peak = std::max(peak, std::fabs(x));
        for (float& x : source) x = x / peak * 0.8f;
    } else {
        const size_t chirpLen = rate, pause = rate / 2;
        source.assign(chirpLen + pause, 0.0f);
        const double f0 = 100.0, f1 = 8000.0, duration = double(chirpLen) / rate;
        const double k = std::log(f1 / f0) / duration;
        for (size_t i = 0; i < chirpLen; ++i) {
            const double t = double(i) / rate;
            const double phase = 2.0 * std::numbers::pi * f0 * (std::exp(k * t) - 1.0) / k;
            const double env = std::min({1.0, t * 100.0, (duration - t) * 100.0});
            source[i] = float(0.8 * env * std::sin(phase));
        }
    }

    std::atomic<size_t> pos{0};
    ws::RenderStream out;
    ws::RenderStream::Options ro;
    ro.sampleRate = rate;
    ro.channels = 2;
    auto fill = [&](float* buf, uint32_t frames) {
        size_t p = pos.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; ++i) {
            const float s = source[p] * gain;
            buf[i * 2] = s;
            buf[i * 2 + 1] = s;
            p = (p + 1) % source.size();
        }
        pos.store(p, std::memory_order_relaxed);
    };
    if (!out.open(dev.Get(), ro, fill, error) || !out.start(error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 4;
    }
    std::printf("playing %s into %s for %.1f s\n",
                !args["wav"].as<std::string>().empty() ? "wav"
                : args.contains("noise")               ? "noise"
                                                       : "chirp",
                ws::toUtf8(ws::describeDevice(dev.Get()).name).c_str(), args["seconds"].as<double>());
    std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(args["seconds"].as<double>() * 1000)));
    out.stop();
    if (!out.lastError().empty()) std::fprintf(stderr, "render error: %s\n", out.lastError().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bomboec::tools::useUtf8Console();
    // Исключения (разбор аргументов cxxopts, std): сообщение вместо abort.
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
