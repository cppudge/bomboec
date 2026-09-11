// bomboec-rec: синхронная запись микрофона и loopback-reference в WAV.
//
//   bomboec-rec --list
//   bomboec-rec --out recordings/take1 --seconds 20 [--mic <id>] [--speakers <id>] [--no-raw]
//
// Результат: mic.wav (mono), ref.wav (stereo), оба 48 kHz float32, выровненные
// по QPC (сэмпл N в обоих файлах соответствует одному моменту времени с
// точностью до дрейфа часов), и metadata.json со статистикой.

#include "core/packet_assembler.h"
#include "core/ring_buffer.h"
#include "core/utf8.h"
#include "core/wav.h"
#include "tools/console.h"
#include "wasapi/capture_stream.h"
#include "wasapi/devices.h"
#include "wasapi/render_stream.h"

#include <cxxopts.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace bomboec;
namespace ws = bomboec::wasapi;

namespace {

constexpr uint32_t kRate = 48000;
constexpr double kTps = 1e7;

struct Track {
    const char* name;
    uint32_t channels;
    RingBuffer ring;
    PacketAssembler assembler;
    WavWriter writer;
    uint64_t skip = 0;  // сэмплы до общего t0 (уменьшается при drain)
    uint64_t initialSkip = 0;
    uint64_t written = 0;
    std::atomic<uint64_t> packets{0};  // читается из главного потока, пока идёт запись
    std::vector<float> scratch;

    Track(const char* n, uint32_t ch) : name(n), channels(ch) {
        ring.resize(ch, kRate * 5);  // 5 с запаса
        assembler.configure(kRate, kTps, &ring);
        scratch.resize(size_t(kRate) * ch);
    }

    void onPacket(const ws::CapturePacket& p) {
        packets.fetch_add(1);
        assembler.push(p.interleaved, p.frames, p.qpc100ns, {p.timestampError, p.discontinuity});
    }

    // Переливает из ring в WAV, пропуская сэмплы до t0.
    void drain() {
        const uint32_t maxFrames = uint32_t(scratch.size() / channels);
        for (;;) {
            const uint32_t n = ring.read(scratch.data(), maxFrames);
            if (n == 0) break;
            uint32_t offset = 0;
            if (skip > 0) {
                const uint64_t s = std::min<uint64_t>(skip, n);
                skip -= s;
                offset = uint32_t(s);
            }
            if (n > offset) {
                written += writer.write(scratch.data() + size_t(offset) * channels, n - offset);
            }
        }
    }
};

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

void listDevices() {
    std::string error;
    for (const ws::Flow flow : {ws::Flow::Capture, ws::Flow::Render}) {
        std::printf("%s:\n", flow == ws::Flow::Capture ? "Capture" : "Render");
        for (const ws::DeviceInfo& d : ws::enumerateDevices(flow, error)) {
            std::printf("  %s %s\n      id: %s\n", d.isDefault ? "*" : " ", ws::toUtf8(d.name).c_str(),
                        ws::toUtf8(d.id).c_str());
        }
        if (!error.empty()) std::printf("  error: %s\n", error.c_str());
    }
}

int run(int argc, char** argv) {
    cxxopts::Options opts("bomboec-rec", "Synchronized mic + loopback recorder");
    // clang-format off
    opts.add_options()
        ("list", "List audio endpoints")
        ("out", "Output directory", cxxopts::value<std::string>())
        ("seconds", "Duration in seconds", cxxopts::value<double>()->default_value("15"))
        ("mic", "Capture endpoint id (default: system default)", cxxopts::value<std::string>()->default_value(""))
        ("speakers", "Render endpoint id for loopback (default: system default)", cxxopts::value<std::string>()->default_value(""))
        ("no-raw", "Do not request raw mode for the microphone")
        ("no-keepalive", "Do not play silence to keep loopback alive")
        ("h,help", "Help");
    // clang-format on
    auto args = opts.parse(argc, argv);
    if (args.contains("help")) {
        std::printf("%s\n", opts.help().c_str());
        return 0;
    }

    const ws::ComInit com;
    if (args.contains("list")) {
        listDevices();
        return 0;
    }
    if (!args.contains("out")) {
        std::fprintf(stderr, "--out is required\n%s\n", opts.help().c_str());
        return 1;
    }

    const std::filesystem::path outDir = pathFromUtf8(args["out"].as<std::string>());
    const double seconds = args["seconds"].as<double>();
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    std::string error;
    const ws::ComPtr<IMMDevice> micDev =
        ws::openDevice(ws::Flow::Capture, ws::fromUtf8(args["mic"].as<std::string>()), error);
    if (!micDev) {
        std::fprintf(stderr, "mic: %s\n", error.c_str());
        return 2;
    }
    const ws::ComPtr<IMMDevice> spkDev =
        ws::openDevice(ws::Flow::Render, ws::fromUtf8(args["speakers"].as<std::string>()), error);
    if (!spkDev) {
        std::fprintf(stderr, "speakers: %s\n", error.c_str());
        return 2;
    }
    const ws::DeviceInfo micInfo = ws::describeDevice(micDev.Get());
    const ws::DeviceInfo spkInfo = ws::describeDevice(spkDev.Get());

    Track mic("mic", 1);
    Track ref("ref", 2);
    if (!mic.writer.open(outDir / "mic.wav", 1, kRate, error) ||
        !ref.writer.open(outDir / "ref.wav", 2, kRate, error)) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 3;
    }

    ws::CaptureStream micStream, refStream;
    ws::CaptureStream::Options micOpt;
    micOpt.channels = 1;
    micOpt.raw = !args.contains("no-raw");
    ws::CaptureStream::Options refOpt;
    refOpt.channels = 2;
    refOpt.loopback = true;
    if (!micStream.open(micDev.Get(), micOpt, [&](const ws::CapturePacket& p) { mic.onPacket(p); }, error)) {
        std::fprintf(stderr, "mic open: %s\n", error.c_str());
        return 4;
    }
    if (!refStream.open(spkDev.Get(), refOpt, [&](const ws::CapturePacket& p) { ref.onPacket(p); }, error)) {
        std::fprintf(stderr, "loopback open: %s\n", error.c_str());
        return 4;
    }

    ws::RenderStream keepalive;
    bool keepaliveOn = false;
    if (!args.contains("no-keepalive")) {
        const ws::RenderStream::Options ro;
        if (keepalive.open(spkDev.Get(), ro, nullptr, error) && keepalive.start(error)) {
            keepaliveOn = true;
        } else {
            std::fprintf(stderr, "keepalive render disabled: %s\n", error.c_str());
        }
    }

    std::printf("mic:      %s (raw %s, engine channels %u, %s)\n", ws::toUtf8(micInfo.name).c_str(),
                micStream.rawApplied() ? "on" : "off", micStream.deviceChannels(),
                micStream.eventDriven() ? "event" : "polling");
    std::printf("loopback: %s (engine channels %u, %s, keepalive %s)\n", ws::toUtf8(spkInfo.name).c_str(),
                refStream.deviceChannels(), refStream.eventDriven() ? "event" : "polling", keepaliveOn ? "on" : "off");

    if (!micStream.start(error) || !refStream.start(error)) {
        std::fprintf(stderr, "start: %s\n", error.c_str());
        return 5;
    }

    // Ждём первый пакет с обоих потоков, затем общий t0 = более поздний origin.
    const auto startWall = std::chrono::steady_clock::now();
    while (!(mic.assembler.started() && ref.assembler.started())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (std::chrono::steady_clock::now() - startWall > std::chrono::seconds(5)) {
            std::fprintf(stderr, "no packets: mic %llu, loopback %llu (mic err: %s; loopback err: %s)\n",
                         (unsigned long long)mic.packets.load(), (unsigned long long)ref.packets.load(),
                         micStream.lastError().c_str(), refStream.lastError().c_str());
            return 6;
        }
    }
    const int64_t t0 = std::max(mic.assembler.originTicks(), ref.assembler.originTicks());
    for (Track* t : {&mic, &ref}) {
        t->skip = uint64_t(std::llround(double(t0 - t->assembler.originTicks()) * kRate / kTps));
        t->initialSkip = t->skip;
    }
    std::printf("t0 aligned: mic skips %llu samples, ref skips %llu samples\n", (unsigned long long)mic.skip,
                (unsigned long long)ref.skip);

    const auto deadline = startWall + std::chrono::milliseconds(int64_t(seconds * 1000));
    auto nextReport = startWall + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        mic.drain();
        ref.drain();
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextReport) {
            nextReport += std::chrono::seconds(1);
            const double elapsed = std::chrono::duration<double>(now - startWall).count();
            std::printf("\r%5.1f s  mic %llu  ref %llu  gaps %llu/%llu  drift %+.1f/%+.1f ppm   ", elapsed,
                        (unsigned long long)mic.written, (unsigned long long)ref.written,
                        (unsigned long long)mic.assembler.statsSnapshot().gaps,
                        (unsigned long long)ref.assembler.statsSnapshot().gaps,
                        mic.assembler.timelineSnapshot().driftPpm(), ref.assembler.timelineSnapshot().driftPpm());
            std::fflush(stdout);
        }
        if (!micStream.lastError().empty() || !refStream.lastError().empty()) {
            std::fprintf(stderr, "\nstream error: mic '%s' loopback '%s'\n", micStream.lastError().c_str(),
                         refStream.lastError().c_str());
            break;
        }
    }
    micStream.stop();
    refStream.stop();
    keepalive.stop();
    mic.drain();
    ref.drain();
    mic.writer.close();
    ref.writer.close();
    std::printf("\n");

    // metadata.json
    std::FILE* f = nullptr;
    _wfopen_s(&f, (outDir / "metadata.json").wstring().c_str(), L"wb");
    if (f) {
        std::fprintf(f, "{\n  \"sample_rate\": %u,\n", kRate);
        std::fprintf(f, "  \"mic_device\": \"%s\",\n  \"mic_device_id\": \"%s\",\n",
                     jsonEscape(ws::toUtf8(micInfo.name)).c_str(), jsonEscape(ws::toUtf8(micInfo.id)).c_str());
        std::fprintf(f, "  \"render_device\": \"%s\",\n  \"render_device_id\": \"%s\",\n",
                     jsonEscape(ws::toUtf8(spkInfo.name)).c_str(), jsonEscape(ws::toUtf8(spkInfo.id)).c_str());
        std::fprintf(f, "  \"mic_raw\": %s,\n  \"keepalive\": %s,\n", micStream.rawApplied() ? "true" : "false",
                     keepaliveOn ? "true" : "false");
        for (Track* t : {&mic, &ref}) {
            const PacketAssembler::Stats& s = t->assembler.stats();
            std::fprintf(f,
                         "  \"%s\": {\"channels\": %u, \"frames\": %llu, \"packets\": %llu, \"skip\": %llu,\n"
                         "    \"gaps\": %llu, \"gap_samples\": %llu, \"overlaps\": %llu, \"overlap_samples\": %llu,\n"
                         "    \"dropped\": %llu, \"max_jitter_ms\": %.3f, \"timestamp_errors\": %llu,\n"
                         "    \"discontinuities\": %llu, \"resyncs\": %llu, \"estimated_rate\": %.3f, "
                         "\"drift_ppm\": %.2f},\n",
                         t->name, t->channels, (unsigned long long)t->written, (unsigned long long)s.packets,
                         (unsigned long long)t->initialSkip, (unsigned long long)s.gaps,
                         (unsigned long long)s.gapSamples, (unsigned long long)s.overlaps,
                         (unsigned long long)s.overlapSamples, (unsigned long long)s.dropped, s.maxJitterMs,
                         (unsigned long long)s.timestampErrors, (unsigned long long)s.discontinuities,
                         (unsigned long long)s.resyncs, t->assembler.timeline().estimatedRate(),
                         t->assembler.timeline().driftPpm());
        }
        std::fprintf(f, "  \"relative_drift_ppm\": %.2f\n}\n",
                     mic.assembler.timeline().driftPpm() - ref.assembler.timeline().driftPpm());
        std::fclose(f);
    }

    std::printf("done: mic %llu frames, ref %llu frames -> %s\n", (unsigned long long)mic.written,
                (unsigned long long)ref.written, pathToUtf8(outDir).c_str());
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
