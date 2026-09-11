#pragma once

#include <cstdint>

namespace bomboec {

// Связь между host-часами (QPC-тики) и индексом сэмпла в потоке устройства.
// Producer добавляет якоря (тик, индекс сэмпла) из меток пакетов WASAPI,
// consumer переводит время в индекс сэмпла и обратно. Оценка реальной
// частоты дискретизации берётся по первому и последнему якорю, когда между
// ними накопилось достаточно времени; иначе используется номинал.
//
// Класс не потокобезопасен: в realtime-движке якоря обновляются и читаются
// под seqlock/копией (этап 4).
class Timeline {
public:
    Timeline() = default;
    Timeline(double nominalRate, double ticksPerSecond)
        : nominalRate_(nominalRate), ticksPerSecond_(ticksPerSecond) {}

    void configure(double nominalRate, double ticksPerSecond) {
        nominalRate_ = nominalRate;
        ticksPerSecond_ = ticksPerSecond;
        reset();
    }

    void reset() { anchors_ = 0; }
    bool valid() const { return anchors_ > 0; }

    // Минимальный интервал между первым и последним якорем, после которого
    // оценка частоты считается достовернее номинала.
    void setMinRateSpanSeconds(double s) { minRateSpanSec_ = s; }

    void anchor(int64_t ticks, uint64_t sampleIndex) {
        if (anchors_ == 0) {
            firstTicks_ = ticks;
            firstSample_ = sampleIndex;
        }
        lastTicks_ = ticks;
        lastSample_ = sampleIndex;
        ++anchors_;
    }

    double nominalRate() const { return nominalRate_; }

    // Оценка реальной частоты (Гц).
    double estimatedRate() const {
        if (anchors_ < 2) return nominalRate_;
        const double span = double(lastTicks_ - firstTicks_) / ticksPerSecond_;
        if (span < minRateSpanSec_) return nominalRate_;
        return double(lastSample_ - firstSample_) / span;
    }

    double driftPpm() const { return (estimatedRate() / nominalRate_ - 1.0) * 1e6; }

    // Дробный индекс сэмпла, соответствующий моменту ticks (экстраполяция от
    // последнего якоря по оценённой частоте).
    double sampleAt(int64_t ticks) const {
        const double dt = double(ticks - lastTicks_) / ticksPerSecond_;
        return double(lastSample_) + dt * estimatedRate();
    }

    int64_t ticksAt(double sampleIndex) const {
        const double ds = sampleIndex - double(lastSample_);
        const double t = ds / estimatedRate() * ticksPerSecond_;
        return lastTicks_ + int64_t(t + (t >= 0 ? 0.5 : -0.5));
    }

    int64_t lastTicks() const { return lastTicks_; }
    uint64_t lastSample() const { return lastSample_; }

private:
    double nominalRate_ = 48000.0;
    double ticksPerSecond_ = 1.0;
    double minRateSpanSec_ = 2.0;
    uint64_t anchors_ = 0;
    int64_t firstTicks_ = 0;
    int64_t lastTicks_ = 0;
    uint64_t firstSample_ = 0;
    uint64_t lastSample_ = 0;
};

}  // namespace bomboec
