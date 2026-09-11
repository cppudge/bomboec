// bomboec-play: воспроизведение тестового сигнала или WAV в render endpoint.
//
//   bomboec-play --output <id> --seconds 10 [--wav file.wav] [--gain-db -12]
//
// Без --wav играет чирп 100..8000 Hz (1 с) с паузой 0.5 с.
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
    std::printf("playing %s into %s for %.1f s\n", args["wav"].as<std::string>().empty() ? "chirp" : "wav",
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
