#pragma once

#include "core/ring_buffer.h"
#include "core/timeline.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace bomboec {

// Превращает поток пакетов с метками времени (WASAPI: QPC-позиция первого
// сэмпла пакета) в непрерывный поток сэмплов в RingBuffer:
//  - пропуск между ожидаемым и реальным временем пакета больше порога
//    заполняется тишиной (loopback без активных render-потоков, стойлы);
//  - наложение (пакет пришёл раньше ожидаемого) отбрасывает лишние сэмплы;
//  - каждый пакет становится якорем Timeline (индекс сэмпла <-> тики).
//
// Ожидаемое время следующего пакета переустанавливается от метки каждого
// пакета, поэтому дрейф часов устройства не накапливается в ошибку:
// он проявляется только в Timeline::driftPpm(). Цена: разрыв короче порога
// не обнаруживается. Порог должен быть больше джиттера меток.
//
// Вызывается из потока-producer соответствующего RingBuffer.
class PacketAssembler {
public:
    struct Stats {
        uint64_t packets = 0;
        uint64_t gaps = 0;
        uint64_t gapSamples = 0;
        uint64_t overlaps = 0;
        uint64_t overlapSamples = 0;
        uint64_t dropped = 0;         // не поместилось в ring
        double maxJitterMs = 0.0;     // максимальное |ожидаемое - реальное| ниже порога
    };

    void configure(double nominalRate, double ticksPerSecond, RingBuffer* ring,
                   double gapThresholdMs = 2.5) {
        rate_ = nominalRate;
        tps_ = ticksPerSecond;
        ring_ = ring;
        thresholdSamples_ = int64_t(gapThresholdMs / 1000.0 * nominalRate + 0.5);
        timeline_.configure(nominalRate, ticksPerSecond);
        reset();
    }

    void reset() {
        SpinGuard g(lock_);
        started_.store(false, std::memory_order_release);
        originTicks_ = 0;
        expectedTicks_ = 0;
        stats_ = {};
        timeline_.reset();
    }

    // interleaved содержит frames фреймов с числом каналов ring->channels().
    void push(const float* interleaved, uint32_t frames, int64_t ticks) {
        ++stats_.packets;
        if (!started_.load(std::memory_order_relaxed)) {
            originTicks_ = ticks;
            expectedTicks_ = ticks;
        }

        const int64_t deltaTicks = ticks - expectedTicks_;
        const int64_t deltaSamples = int64_t(double(deltaTicks) * rate_ / tps_ + (deltaTicks >= 0 ? 0.5 : -0.5));
        uint32_t skip = 0;
        if (deltaSamples > thresholdSamples_) {
            const uint32_t fill = uint32_t(deltaSamples);
            const uint32_t written = ring_->writeSilence(fill);
            stats_.dropped += fill - written;
            ++stats_.gaps;
            stats_.gapSamples += fill;
        } else if (deltaSamples < -thresholdSamples_) {
            skip = uint32_t(std::min<int64_t>(-deltaSamples, frames));
            ++stats_.overlaps;
            stats_.overlapSamples += skip;
        } else {
            stats_.maxJitterMs = std::max(stats_.maxJitterMs, std::abs(double(deltaTicks)) / tps_ * 1000.0);
        }

        const uint32_t n = frames - skip;
        if (n > 0) {
            const uint32_t written = ring_->write(interleaved + size_t(skip) * ring_->channels(), n);
            stats_.dropped += n - written;
        }

        // Якорь: первый записанный сэмпл этого пакета соответствует моменту
        // ticks + skip/rate. Индекс берём по позиции записи ring.
        const uint64_t firstIndex = ring_->totalWritten() - n;
        {
            SpinGuard g(lock_);
            timeline_.anchor(ticks + int64_t(double(skip) / rate_ * tps_ + 0.5), firstIndex);
        }
        started_.store(true, std::memory_order_release);

        expectedTicks_ = ticks + int64_t(double(frames) / rate_ * tps_ + 0.5);
    }

    // Потокобезопасно относительно push(): читается из потока-consumer.
    bool started() const { return started_.load(std::memory_order_acquire); }
    int64_t originTicks() const { return originTicks_; }
    Timeline timelineSnapshot() const {
        SpinGuard g(lock_);
        return timeline_;
    }
    // Только из потока-producer или когда push() не вызывается.
    const Timeline& timeline() const { return timeline_; }
    const Stats& stats() const { return stats_; }

private:
    struct SpinGuard {
        explicit SpinGuard(std::atomic_flag& f) : f_(f) {
            while (f_.test_and_set(std::memory_order_acquire)) {}
        }
        ~SpinGuard() { f_.clear(std::memory_order_release); }
        std::atomic_flag& f_;
    };

    double rate_ = 48000.0;
    double tps_ = 1e7;
    RingBuffer* ring_ = nullptr;
    int64_t thresholdSamples_ = 120;
    Timeline timeline_;
    mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    std::atomic<bool> started_{false};
    int64_t originTicks_ = 0;
    int64_t expectedTicks_ = 0;
    Stats stats_;
};

}  // namespace bomboec
