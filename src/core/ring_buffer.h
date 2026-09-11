#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace bomboec {

// Lock-free SPSC кольцевой буфер interleaved-фреймов (frame = channels сэмплов).
// Один поток пишет, один читает. Позиции абсолютные (64 бит), поэтому
// totalWritten()/totalRead() можно использовать как индекс сэмпла в потоке
// для привязки к таймлайну.
class RingBuffer {
public:
    RingBuffer() = default;
    RingBuffer(uint32_t channels, uint32_t capacityFrames) { resize(channels, capacityFrames); }

    // Не потокобезопасно: вызывать до старта или когда обе стороны остановлены.
    void resize(uint32_t channels, uint32_t capacityFrames) {
        channels_ = channels;
        capacity_ = capacityFrames;
        buffer_.assign(size_t(channels) * capacityFrames, 0.0f);
        writePos_.store(0, std::memory_order_relaxed);
        readPos_.store(0, std::memory_order_relaxed);
    }

    // Consumer: отбросить всё накопленное.
    void reset() {
        readPos_.store(writePos_.load(std::memory_order_acquire), std::memory_order_release);
    }

    uint32_t channels() const { return channels_; }
    uint32_t capacity() const { return capacity_; }

    uint32_t readable() const {
        return uint32_t(writePos_.load(std::memory_order_acquire) -
                        readPos_.load(std::memory_order_acquire));
    }
    uint32_t writable() const { return capacity_ - readable(); }

    uint64_t totalWritten() const { return writePos_.load(std::memory_order_acquire); }
    uint64_t totalRead() const { return readPos_.load(std::memory_order_acquire); }

    // Producer. Возвращает число записанных фреймов (меньше frames при переполнении).
    uint32_t write(const float* interleaved, uint32_t frames) {
        const uint64_t w = writePos_.load(std::memory_order_relaxed);
        const uint64_t r = readPos_.load(std::memory_order_acquire);
        const uint32_t free = capacity_ - uint32_t(w - r);
        const uint32_t n = std::min(frames, free);
        copyIn(w, interleaved, n);
        writePos_.store(w + n, std::memory_order_release);
        return n;
    }

    // Producer: записать frames фреймов тишины (заполнение пропусков по таймлайну).
    uint32_t writeSilence(uint32_t frames) {
        const uint64_t w = writePos_.load(std::memory_order_relaxed);
        const uint64_t r = readPos_.load(std::memory_order_acquire);
        const uint32_t free = capacity_ - uint32_t(w - r);
        const uint32_t n = std::min(frames, free);
        uint32_t done = 0;
        while (done < n) {
            const uint32_t idx = uint32_t((w + done) % capacity_);
            const uint32_t chunk = std::min(n - done, capacity_ - idx);
            std::memset(buffer_.data() + size_t(idx) * channels_, 0,
                        size_t(chunk) * channels_ * sizeof(float));
            done += chunk;
        }
        writePos_.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer. Возвращает число прочитанных фреймов.
    uint32_t read(float* interleaved, uint32_t frames) {
        const uint64_t r = readPos_.load(std::memory_order_relaxed);
        const uint64_t w = writePos_.load(std::memory_order_acquire);
        const uint32_t n = std::min(frames, uint32_t(w - r));
        copyOut(r, interleaved, n);
        readPos_.store(r + n, std::memory_order_release);
        return n;
    }

    // Consumer: прочитать без сдвига позиции.
    uint32_t peek(float* interleaved, uint32_t frames) const {
        const uint64_t r = readPos_.load(std::memory_order_relaxed);
        const uint64_t w = writePos_.load(std::memory_order_acquire);
        const uint32_t n = std::min(frames, uint32_t(w - r));
        copyOut(r, interleaved, n);
        return n;
    }

    // Consumer: отбросить frames самых старых фреймов.
    uint32_t discard(uint32_t frames) {
        const uint64_t r = readPos_.load(std::memory_order_relaxed);
        const uint64_t w = writePos_.load(std::memory_order_acquire);
        const uint32_t n = std::min(frames, uint32_t(w - r));
        readPos_.store(r + n, std::memory_order_release);
        return n;
    }

private:
    void copyIn(uint64_t pos, const float* src, uint32_t frames) {
        uint32_t done = 0;
        while (done < frames) {
            const uint32_t idx = uint32_t((pos + done) % capacity_);
            const uint32_t chunk = std::min(frames - done, capacity_ - idx);
            std::memcpy(buffer_.data() + size_t(idx) * channels_,
                        src + size_t(done) * channels_,
                        size_t(chunk) * channels_ * sizeof(float));
            done += chunk;
        }
    }

    void copyOut(uint64_t pos, float* dst, uint32_t frames) const {
        uint32_t done = 0;
        while (done < frames) {
            const uint32_t idx = uint32_t((pos + done) % capacity_);
            const uint32_t chunk = std::min(frames - done, capacity_ - idx);
            std::memcpy(dst + size_t(done) * channels_,
                        buffer_.data() + size_t(idx) * channels_,
                        size_t(chunk) * channels_ * sizeof(float));
            done += chunk;
        }
    }

    uint32_t channels_ = 0;
    uint32_t capacity_ = 0;
    std::vector<float> buffer_;
    std::atomic<uint64_t> writePos_{0};
    std::atomic<uint64_t> readPos_{0};
};

}  // namespace bomboec
