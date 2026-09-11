#pragma once

#include <immintrin.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace bomboec {

// Публикация значения одним писателем для любых читателей. Писатель никогда не
// ждёт (важно для аудиопотоков с MMCSS: заблокированный UI-поток не может их
// задержать), читатель повторяет чтение, если попал на запись.
//
// Данные лежат словами std::atomic<uint64_t> с relaxed-доступом, поэтому
// конкурентное чтение не гонка данных в смысле C++ (H.-J. Boehm, "Can seqlocks
// get along with programming language memory models?", 2012).
template <typename T> class SeqLock {
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr size_t kWords = (sizeof(T) + 7) / 8;

public:
    SeqLock() { store(T{}); }

    // Только из одного потока-писателя.
    void store(const T& value) {
        std::array<uint64_t, kWords> words{};
        std::memcpy(words.data(), &value, sizeof(T));
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < kWords; ++i) data_[i].store(words[i], std::memory_order_relaxed);
        seq_.store(s + 2, std::memory_order_release);
    }

    T load() const {
        std::array<uint64_t, kWords> words{};
        for (;;) {
            const uint32_t s = seq_.load(std::memory_order_acquire);
            if ((s & 1) == 0) {
                for (size_t i = 0; i < kWords; ++i) words[i] = data_[i].load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (seq_.load(std::memory_order_relaxed) == s) break;
            }
            _mm_pause();
        }
        T value;
        std::memcpy(&value, words.data(), sizeof(T));
        return value;
    }

private:
    std::atomic<uint32_t> seq_{0};
    std::array<std::atomic<uint64_t>, kWords> data_{};
};

}  // namespace bomboec
