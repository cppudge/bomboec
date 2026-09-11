#pragma once

#include "core/ring_buffer.h"
#include "core/seqlock.h"
#include "core/timeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace bomboec {

// Флаги пакета WASAPI, которые влияют на сборку потока.
struct PacketFlags {
    bool timestampError = false;  // AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR: метке не верить
    bool discontinuity = false;   // AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY: глитч устройства
};

// Превращает поток пакетов с метками времени (WASAPI: QPC-позиция первого
// сэмпла пакета) в непрерывный поток сэмплов в RingBuffer:
//  - пропуск между ожидаемым и реальным временем пакета больше порога
//    заполняется тишиной (loopback без активных render-потоков, стойлы);
//  - наложение (пакет пришёл раньше ожидаемого) отбрасывает лишние сэмплы;
//  - расхождение больше порога ресинхронизации (устройство перезапустилось,
//    битая метка) не заливается тишиной и не режется: Timeline начинается
//    заново с этого пакета (stats().resyncs). Иначе одна метка qpc = 0 залила
//    бы тишиной всё кольцо;
//  - пакет с timestampError встаёт вплотную к предыдущему;
//  - каждый записанный пакет становится якорем Timeline (индекс сэмпла <-> тики).
//
// Ожидаемое время следующего пакета переустанавливается от метки каждого
// пакета, поэтому дрейф часов устройства не накапливается в ошибку:
// он проявляется только в Timeline::driftPpm(). Цена: разрыв короче порога
// не обнаруживается. Порог должен быть больше джиттера меток.
//
// push() вызывается из потока-producer соответствующего RingBuffer. Таймлайн и
// статистику он публикует через SeqLock: другие потоки читают снимки и никогда
// не задерживают producer.
class PacketAssembler {
public:
    struct Stats {
        uint64_t packets = 0;
        uint64_t gaps = 0;
        uint64_t gapSamples = 0;
        uint64_t overlaps = 0;
        uint64_t overlapSamples = 0;
        uint64_t resyncs = 0;          // расхождение больше порога ресинхронизации
        uint64_t timestampErrors = 0;  // пакетов с timestampError
        uint64_t discontinuities = 0;  // пакетов с discontinuity
        uint64_t dropped = 0;          // не поместилось в ring
        double maxJitterMs = 0.0;      // максимальное |ожидаемое - реальное| ниже порога
    };

    // Как reset(): только когда push() не вызывается.
    void configure(double nominalRate, double ticksPerSecond, RingBuffer* ring, double gapThresholdMs = 2.5,
                   double resyncThresholdMs = 200.0) {
        rate_ = nominalRate;
        tps_ = ticksPerSecond;
        ring_ = ring;
        thresholdSamples_ = std::llround(gapThresholdMs / 1000.0 * nominalRate);
        resyncSamples_ = std::llround(resyncThresholdMs / 1000.0 * nominalRate);
        timeline_.configure(nominalRate, ticksPerSecond);
        reset();
    }

    void reset() {
        started_.store(false, std::memory_order_release);
        originTicks_ = 0;
        expectedTicks_ = 0;
        stats_ = {};
        timeline_.reset();
        publishedTimeline_.store(timeline_);
        publishedStats_.store(stats_);
    }

    // interleaved содержит frames фреймов с числом каналов ring->channels().
    void push(const float* interleaved, uint32_t frames, int64_t ticks, PacketFlags flags = {}) {
        ++stats_.packets;
        if (flags.discontinuity) ++stats_.discontinuities;
        const bool started = started_.load(std::memory_order_relaxed);
        if (flags.timestampError) {
            ++stats_.timestampErrors;
            if (started) ticks = expectedTicks_;
        }
        if (!started) {
            originTicks_ = ticks;
            expectedTicks_ = ticks;
        }

        const int64_t deltaTicks = ticks - expectedTicks_;
        const int64_t deltaSamples = std::llround(double(deltaTicks) * rate_ / tps_);
        uint32_t skip = 0;
        const bool resync = std::llabs(deltaSamples) > resyncSamples_;
        if (resync) {
            ++stats_.resyncs;
        } else if (deltaSamples > thresholdSamples_) {
            const auto fill = uint32_t(deltaSamples);  // не больше resyncSamples_
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
        uint32_t written = 0;
        if (n > 0) {
            written = ring_->write(interleaved + size_t(skip) * ring_->channels(), n);
            stats_.dropped += n - written;
        }

        // Якорь: первый записанный сэмпл пакета соответствует моменту ticks + skip/rate.
        // write() при переполнении отбрасывает хвост пакета, поэтому индекс считается от
        // записанного. Если не записалось ничего, totalWritten() принадлежит будущему
        // пакету и якорь не ставится (кроме ресинхронизации: там отсчёт начинается заново).
        if (written > 0 || resync) {
            const uint64_t firstIndex = ring_->totalWritten() - written;
            if (resync) timeline_.reset();
            timeline_.anchor(ticks + std::llround(double(skip) / rate_ * tps_), firstIndex);
            publishedTimeline_.store(timeline_);
            started_.store(true, std::memory_order_release);
        }
        publishedStats_.store(stats_);

        expectedTicks_ = ticks + std::llround(double(frames) / rate_ * tps_);
    }

    // Из любого потока.
    bool started() const { return started_.load(std::memory_order_acquire); }
    Timeline timelineSnapshot() const { return publishedTimeline_.load(); }
    Stats statsSnapshot() const { return publishedStats_.load(); }

    // Только из потока-producer или когда push() не вызывается.
    int64_t originTicks() const { return originTicks_; }
    const Timeline& timeline() const { return timeline_; }
    const Stats& stats() const { return stats_; }

private:
    double rate_ = 48000.0;
    double tps_ = 1e7;
    RingBuffer* ring_ = nullptr;
    int64_t thresholdSamples_ = 120;
    int64_t resyncSamples_ = 9600;
    Timeline timeline_;
    SeqLock<Timeline> publishedTimeline_;
    SeqLock<Stats> publishedStats_;
    std::atomic<bool> started_{false};
    int64_t originTicks_ = 0;
    int64_t expectedTicks_ = 0;
    Stats stats_;
};

}  // namespace bomboec
