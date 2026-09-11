// bomboec-run: консольный запуск realtime-движка.
//
//   bomboec-run --config config/default.toml [--seconds 30] [--mic <id>] [--speakers <id>]
//               [--output <id>] [--record dir]
//
// Идентификаторы из аргументов имеют приоритет над [devices] конфига.

#include "engine/engine.h"
#include "wasapi/com_util.h"

#include <cxxopts.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace bomboec;

int main(int argc, char** argv) {
    cxxopts::Options opts("bomboec-run", "Realtime AEC engine (console)");
    // clang-format off
    opts.add_options()
        ("config", "Pipeline TOML", cxxopts::value<std::string>()->default_value("config/default.toml"))
        ("seconds", "Run time, 0 = until Ctrl+C", cxxopts::value<double>()->default_value("0"))
        ("mic", "Capture endpoint id", cxxopts::value<std::string>())
        ("speakers", "Render endpoint id for loopback", cxxopts::value<std::string>())
        ("output", "Render endpoint id for processed output", cxxopts::value<std::string>())
        ("record", "Debug recording directory (mic_raw/ref/out .wav)", cxxopts::value<std::string>())
        ("h,help", "Help");
    // clang-format on
    auto args = opts.parse(argc, argv);
    if (args.count("help")) {
        std::printf("%s\n", opts.help().c_str());
        return 0;
    }

    wasapi::ComInit com;
    std::string error;
    AppConfig cfg;
    if (!loadConfig(args["config"].as<std::string>(), cfg, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (args.count("mic")) cfg.engine.micId = args["mic"].as<std::string>();
    if (args.count("speakers")) cfg.engine.speakersId = args["speakers"].as<std::string>();
    if (args.count("output")) cfg.engine.outputId = args["output"].as<std::string>();
    if (args.count("record")) cfg.engine.recordDir = args["record"].as<std::string>();

    Engine engine;
    if (!engine.start(cfg, error)) {
        std::fprintf(stderr, "engine start failed: %s\n", error.c_str());
        return 2;
    }
    {
        const EngineStatus s = engine.status();
        std::printf("mic:      %s (raw %s)\nloopback: %s\noutput:   %s\nreference lead %.0f ms\n",
                    s.micName.c_str(), s.micRaw ? "on" : "off", s.speakersName.c_str(), s.outputName.c_str(),
                    s.referenceLeadMs);
    }

    const double seconds = args["seconds"].as<double>();
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const EngineStatus s = engine.status();
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::printf("%6.1f s  mic %6.1f  ref %6.1f  out %6.1f dBFS  delay %s  erle %s  refmiss %llu  gaps %llu/%llu  out %u ms (margin %u) under %llu over %llu  fill +%llu/-%llu  drift %+.0f/%+.0f\n",
                    t, s.micDb, s.refDb, s.outDb,
                    s.stats.delayMs ? std::to_string(int(*s.stats.delayMs)).c_str() : "n/a",
                    s.stats.erleDb ? std::to_string(int(*s.stats.erleDb)).c_str() : "n/a",
                    (unsigned long long)s.refMissing, (unsigned long long)s.micGaps, (unsigned long long)s.refGaps,
                    s.outBufferedMs, s.outMarginMs, (unsigned long long)s.outUnderruns, (unsigned long long)s.outOverruns,
                    (unsigned long long)s.outInserted, (unsigned long long)s.outDropped,
                    s.micDriftPpm, s.refDriftPpm);
        std::fflush(stdout);
        if (!s.error.empty()) {
            std::fprintf(stderr, "engine error: %s\n", s.error.c_str());
            break;
        }
        if (seconds > 0 && t >= seconds) break;
    }
    engine.stop();
    return 0;
}
