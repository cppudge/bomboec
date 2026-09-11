// Примитивы realtime-части под двумя потоками. TSan в MSVC нет: стресс-тесты
// ловят нарушение порядка и разорванные значения, пресет asan - ошибки памяти.

#include "core/denormals.h"
#include "core/ring_buffer.h"
#include "core/seqlock.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace bomboec;

TEST_CASE("SeqLock gives a concurrent reader whole values in order", "[realtime]") {
    struct Value {
        uint64_t a = 0, b = 0, c = 0;
        double d = 0.0;
    };
    SeqLock<Value> lock;
    CHECK(lock.load().a == 0);
    lock.store({1, 1, 1, 1.0});
    CHECK(lock.load().c == 1);

    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (uint64_t k = 2; k < 200'000; ++k) lock.store({k, k, k, double(k)});
        done = true;
    });
    uint64_t reads = 0, torn = 0, backwards = 0, last = 0;
    while (!done.load()) {
        const Value v = lock.load();
        if (v.a != v.b || v.b != v.c || double(v.a) != v.d) ++torn;
        if (v.a < last) ++backwards;
        last = v.a;
        ++reads;
    }
    writer.join();
    CHECK(torn == 0);
    CHECK(backwards == 0);
    CHECK(reads > 0);
    CHECK(lock.load().a == 199'999);
}

TEST_CASE("RingBuffer keeps order under a concurrent producer and consumer", "[realtime]") {
    RingBuffer ring(2, 1000);
    constexpr uint32_t kTotal = 2'000'000;
    std::thread producer([&] {
        std::vector<float> chunk(2 * 97);
        uint32_t next = 0;
        while (next < kTotal) {
            const uint32_t n = std::min<uint32_t>(97, kTotal - next);
            for (uint32_t i = 0; i < n; ++i) {
                chunk[2 * i] = float(next + i);
                chunk[2 * i + 1] = -float(next + i);
            }
            const uint32_t written = ring.write(chunk.data(), n);
            next += written;
            if (written == 0) std::this_thread::yield();
        }
    });
    uint32_t expected = 0, errors = 0;
    std::vector<float> buf(2 * 131);
    while (expected < kTotal) {
        const uint32_t n = ring.read(buf.data(), 131);
        for (uint32_t i = 0; i < n; ++i, ++expected) {
            if (buf[2 * i] != float(expected) || buf[2 * i + 1] != -float(expected)) ++errors;
        }
        if (n == 0) std::this_thread::yield();
    }
    producer.join();
    CHECK(errors == 0);
}

TEST_CASE("ScopedFlushDenormals flushes denormals and restores the mode", "[realtime]") {
    const volatile float tiny = 1e-40f;  // denormal
    const volatile float one = 1.0f;
    {
        const ScopedFlushDenormals guard;
        const float r = tiny * one;
        CHECK(r == 0.0f);
    }
    const float r = tiny * one;
    CHECK(r != 0.0f);
}
