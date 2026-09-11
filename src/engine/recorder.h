#pragma once

#include "core/ring_buffer.h"
#include "core/wav.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace bomboec {

// Debug-запись нескольких дорожек в WAV (record_dir). Аудиопоток только кладёт
// кадры в lock-free кольца (push), файлы пишет свой поток: fwrite и диск не
// задерживают поток с MMCSS. Не поместившееся в кольцо считается в dropped().
class Recorder {
public:
    struct Track {
        std::string file;  // имя файла в каталоге записи
        uint32_t channels = 1;
    };

    Recorder();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // До старта аудиопотоков. Файлы создаются сразу, каталог при необходимости тоже.
    bool open(const std::filesystem::path& dir, const std::vector<Track>& tracks, uint32_t sampleRate,
              std::string& error);
    // После остановки аудиопотоков: дописать остаток и закрыть файлы.
    void close();
    bool isOpen() const { return !slots_.empty(); }

    // Аудиопоток (один на дорожку): interleaved-кадры дорожки track.
    void push(size_t track, const float* interleaved, uint32_t frames);

    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Slot {
        RingBuffer ring;
        WavWriter writer;
        std::vector<float> scratch;
    };

    void writerMain();
    void drain();

    std::vector<std::unique_ptr<Slot>> slots_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> dropped_{0};
};

}  // namespace bomboec
