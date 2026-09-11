#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace bomboec {

// Регулятор заполнения выходного кольца между producer'ом (mic-поток) и
// consumer'ом (render-поток), часы которых расходятся на десятки ppm.
//
// Измерение: минимум остатка в кольце после каждого чтения consumer'а за
// окно из windowReads чтений (две корзины, поэтому окно 1..2 длины). Минимум
// после чтения - это реальный запас против опустошения, независимый от фазы
// между записью и чтением.
//
// Управление: пропорциональный регулятор с накоплением дробной части.
// step() на каждый кадр producer'а возвращает, сколько сэмплов добавить (+)
// или убрать (-) из кадра, чтобы запас стремился к target. Коррекция на
// кадр ограничена maxStep, чтобы смена высоты тона осталась незаметной.
// При дрейфе 140 ppm и gain 1/1200 установившаяся ошибка около 2 ms,
// постоянная времени около 12 с.
class FillController {
public:
    static constexpr uint32_t kNone = UINT32_MAX;

    void configure(uint32_t targetFrames, uint32_t windowReads = 100, double gain = 1.0 / 1200.0, int maxStep = 4) {
        target_ = targetFrames;
        windowReads_ = std::max<uint32_t>(windowReads, 1);
        gain_ = gain;
        maxStep_ = std::max(maxStep, 1);
        cur_.store(kNone, std::memory_order_relaxed);
        prev_.store(kNone, std::memory_order_relaxed);
        reads_ = 0;
        acc_ = 0.0;
    }

    uint32_t target() const { return target_; }

    // Consumer: остаток в кольце после чтения.
    void observe(uint32_t remainingFrames) {
        if (remainingFrames < cur_.load(std::memory_order_relaxed)) {
            cur_.store(remainingFrames, std::memory_order_relaxed);
        }
        if (++reads_ >= windowReads_) {
            prev_.store(cur_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            cur_.store(kNone, std::memory_order_relaxed);
            reads_ = 0;
        }
    }

    // Минимальный запас за окно; kNone, пока измерений не было.
    uint32_t marginFrames() const {
        return std::min(cur_.load(std::memory_order_relaxed), prev_.load(std::memory_order_relaxed));
    }

    // Producer, раз на кадр: сколько сэмплов добавить (+) или убрать (-).
    int step() {
        const uint32_t m = marginFrames();
        if (m == kNone) return 0;
        const double err = double(target_) - double(m);  // >0: запаса мало, добавляем
        acc_ += err * gain_;
        acc_ = std::clamp(acc_, -double(maxStep_) - 1.0, double(maxStep_) + 1.0);
        const int d = std::clamp(int(acc_), -maxStep_, maxStep_);
        acc_ -= d;
        return d;
    }

private:
    uint32_t target_ = 0;
    uint32_t windowReads_ = 100;
    double gain_ = 1.0 / 1200.0;
    int maxStep_ = 4;
    std::atomic<uint32_t> cur_{kNone}, prev_{kNone};
    uint32_t reads_ = 0;  // только consumer
    double acc_ = 0.0;    // только producer
};

// Растяжение/сжатие кадра линейной интерполяцией: n входных сэмплов -> m
// выходных. Разница в 1..4 сэмпла на 480 меняет высоту тона на 0.2..0.8 %
// в течение 10 ms, без разрыва сигнала: out[0] = in[0], out[m-1] ~ in[n-1].
inline void stretchLinear(const float* in, uint32_t n, float* out, uint32_t m) {
    if (n == 0 || m == 0) return;
    if (n == m) {
        std::copy(in, in + n, out);
        return;
    }
    for (uint32_t i = 0; i < m; ++i) {
        const double x = double(i) * n / m;  // [0, n)
        const uint32_t k = uint32_t(x);
        const float f = float(x - k);
        const float a = in[k];
        const float b = in[std::min(k + 1, n - 1)];
        out[i] = a + (b - a) * f;
    }
}

}  // namespace bomboec
