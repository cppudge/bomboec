#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace bomboec {

// Связь между host-часами (QPC-тики) и индексом сэмпла в потоке устройства.
// Producer добавляет якоря (тик, индекс сэмпла) из меток пакетов WASAPI,
// consumer переводит время в индекс сэмпла и обратно через снимок (Snapshot).
//
// Оценка: линейная регрессия по окну последних kWindow якорей (около 5 с при
// пакетах 10 ms). Наклон даёт реальную частоту дискретизации, а прямая - индекс
// в любой момент времени без джиттера отдельной метки: у USB-микрофона метка
// пакета гуляет до 0.2 ms (10 сэмплов), регрессия по 500 якорям усредняет это
// до долей сэмпла. Пока окно короче minRateSpanSeconds, частота номинальная и
// отсчёт идёт от последнего якоря.
//
// Класс не потокобезопасен: PacketAssembler обновляет свою копию и публикует
// снимок через SeqLock, другие потоки читают снимки.
class Timeline {
public:
    static constexpr uint32_t kWindow = 512;

    // Отображение время <-> индекс сэмпла: тривиально копируемое, для SeqLock.
    struct Snapshot {
        double nominalRate = 48000.0;
        double ticksPerSecond = 1.0;
        double rate = 48000.0;  // оценка реальной частоты, Гц
        double meanX = 0.0;     // центр окна регрессии: секунды от lastTicks
        double meanY = 0.0;     // и сэмплы от lastSample
        int64_t lastTicks = 0;
        uint64_t lastSample = 0;
        uint64_t anchors = 0;

        bool valid() const { return anchors > 0; }
        double estimatedRate() const { return rate; }
        double driftPpm() const { return (rate / nominalRate - 1.0) * 1e6; }

        // Дробный индекс сэмпла в момент ticks (по прямой регрессии).
        double sampleAt(int64_t ticks) const {
            const double x = double(ticks - lastTicks) / ticksPerSecond;
            return double(lastSample) + meanY + (x - meanX) * rate;
        }

        int64_t ticksAt(double sampleIndex) const {
            const double y = sampleIndex - double(lastSample);
            const double t = (meanX + (y - meanY) / rate) * ticksPerSecond;
            return lastTicks + int64_t(t + (t >= 0 ? 0.5 : -0.5));
        }
    };

    Timeline() = default;
    Timeline(double nominalRate, double ticksPerSecond) { configure(nominalRate, ticksPerSecond); }

    void configure(double nominalRate, double ticksPerSecond) {
        snap_.nominalRate = nominalRate;
        snap_.ticksPerSecond = ticksPerSecond;
        reset();
    }

    void reset() {
        snap_.anchors = 0;
        snap_.rate = snap_.nominalRate;
        snap_.meanX = snap_.meanY = 0.0;
    }
    bool valid() const { return snap_.valid(); }

    // Минимальная длина окна, после которой наклон регрессии достовернее номинала.
    void setMinRateSpanSeconds(double s) { minRateSpanSec_ = s; }

    void anchor(int64_t ticks, uint64_t sampleIndex) {
        const uint32_t slot = uint32_t(snap_.anchors % kWindow);
        ticks_[slot] = ticks;
        samples_[slot] = sampleIndex;
        snap_.lastTicks = ticks;
        snap_.lastSample = sampleIndex;
        ++snap_.anchors;
        fit();
    }

    const Snapshot& snapshot() const { return snap_; }

    double nominalRate() const { return snap_.nominalRate; }
    double estimatedRate() const { return snap_.rate; }
    double driftPpm() const { return snap_.driftPpm(); }
    double sampleAt(int64_t ticks) const { return snap_.sampleAt(ticks); }
    int64_t ticksAt(double sampleIndex) const { return snap_.ticksAt(sampleIndex); }
    int64_t lastTicks() const { return snap_.lastTicks; }
    uint64_t lastSample() const { return snap_.lastSample; }

private:
    void fit() {
        const uint64_t n64 = snap_.anchors < kWindow ? snap_.anchors : kWindow;
        const auto n = uint32_t(n64);
        snap_.rate = snap_.nominalRate;
        snap_.meanX = snap_.meanY = 0.0;
        if (n < 2) return;
        // Координаты относительно последнего якоря: секунды и сэмплы.
        const uint32_t oldest = uint32_t((snap_.anchors - n) % kWindow);
        const double span = double(snap_.lastTicks - ticks_[oldest]) / snap_.ticksPerSecond;
        if (span < minRateSpanSec_ || span <= 0.0) return;
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t k = (oldest + i) % kWindow;
            const double x = double(ticks_[k] - snap_.lastTicks) / snap_.ticksPerSecond;
            const double y = double(int64_t(samples_[k] - snap_.lastSample));
            sx += x;
            sy += y;
            sxx += x * x;
            sxy += x * y;
        }
        const double den = double(n) * sxx - sx * sx;
        if (den <= 0.0) return;
        const double slope = (double(n) * sxy - sx * sy) / den;
        if (!(slope > 0.5 * snap_.nominalRate && slope < 2.0 * snap_.nominalRate)) return;  // мусорные метки
        snap_.rate = slope;
        snap_.meanX = sx / double(n);
        snap_.meanY = sy / double(n);
    }

    Snapshot snap_;
    double minRateSpanSec_ = 2.0;
    std::array<int64_t, kWindow> ticks_{};
    std::array<uint64_t, kWindow> samples_{};
};

}  // namespace bomboec
