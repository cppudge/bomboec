#include "engine/recorder.h"

#include <chrono>

namespace bomboec {

namespace {

constexpr uint32_t kRingSeconds = 2;                          // запас кольца на случай медленного диска
constexpr auto kDrainPeriod = std::chrono::milliseconds(20);  // как часто поток записи опустошает кольца

}  // namespace

Recorder::Recorder() = default;
Recorder::~Recorder() { close(); }

bool Recorder::open(const std::filesystem::path& dir, const std::vector<Track>& tracks, uint32_t sampleRate,
                    std::string& error) {
    close();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (const Track& t : tracks) {
        auto slot = std::make_unique<Slot>();
        if (!slot->writer.open(dir / t.file, t.channels, sampleRate, error)) {
            slots_.clear();
            return false;
        }
        slot->ring.resize(t.channels, sampleRate * kRingSeconds);
        slot->scratch.resize(size_t(sampleRate / 10) * t.channels);  // до 100 ms за одну запись
        slots_.push_back(std::move(slot));
    }
    dropped_ = 0;
    stop_ = false;
    thread_ = std::thread([this] { writerMain(); });
    return true;
}

void Recorder::close() {
    if (thread_.joinable()) {
        stop_ = true;
        thread_.join();
    }
    drain();  // то, что аудиопоток успел положить после последнего прохода
    slots_.clear();
}

void Recorder::push(size_t track, const float* interleaved, uint32_t frames) {
    if (track >= slots_.size()) return;
    const uint32_t written = slots_[track]->ring.write(interleaved, frames);
    if (written < frames) dropped_.fetch_add(frames - written, std::memory_order_relaxed);
}

void Recorder::writerMain() {
    while (!stop_.load()) {
        drain();
        std::this_thread::sleep_for(kDrainPeriod);
    }
}

void Recorder::drain() {
    for (const std::unique_ptr<Slot>& s : slots_) {
        const uint32_t maxFrames = uint32_t(s->scratch.size() / s->ring.channels());
        for (;;) {
            const uint32_t n = s->ring.read(s->scratch.data(), maxFrames);
            if (n == 0) break;
            s->writer.write(s->scratch.data(), n);
        }
    }
}

}  // namespace bomboec
