#include "core/fill_controller.h"
#include "core/ring_buffer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace bomboec;

TEST_CASE("stretchLinear keeps endpoints and ramps", "[fill]") {
    std::vector<float> in(480);
    for (size_t i = 0; i < in.size(); ++i) in[i] = float(i);
    std::vector<float> out(490);

    stretchLinear(in.data(), 480, out.data(), 480);
    REQUIRE(out[123] == 123.0f);

    stretchLinear(in.data(), 480, out.data(), 479);
    REQUIRE(out[0] == 0.0f);
    REQUIRE_THAT(out[478], Catch::Matchers::WithinAbs(479.0, 0.01));
    for (size_t i = 1; i < 479; ++i) REQUIRE(out[i] > out[i - 1]);

    stretchLinear(in.data(), 480, out.data(), 484);
    REQUIRE(out[0] == 0.0f);
    REQUIRE_THAT(out[483], Catch::Matchers::WithinAbs(479.0, 0.01));
    for (size_t i = 1; i < 484; ++i) REQUIRE(out[i] > out[i - 1]);
}

TEST_CASE("FillController is idle until observed", "[fill]") {
    FillController fc;
    fc.configure(480);
    REQUIRE(fc.marginFrames() == FillController::kNone);
    REQUIRE(fc.step() == 0);
    fc.observe(2000);
    // Запаса много: убираем, но не быстрее maxStep за кадр.
    int total = 0;
    for (int i = 0; i < 100; ++i) {
        const int d = fc.step();
        REQUIRE(d <= 0);
        REQUIRE(d >= -4);
        total += d;
    }
    REQUIRE(total < 0);
}

// Модель: producer пишет 480 (+d) кадров на шаг, consumer читает 480 кадров,
// но его часы медленнее на drift ppm, т.е. раз в 1/(drift) кадров читает на
// один меньше. Запас должен сойтись к цели и не расти со временем.
static void simulate(double driftPpm, uint32_t target, uint32_t& finalMargin, int64_t& netAdjust) {
    RingBuffer ring(1, 48000 * 2);
    FillController fc;
    fc.configure(target);
    std::vector<float> frame(480 + 8, 0.0f);
    // Consumer с отрицательным дрейфом читает до 481 кадра за шаг.
    std::vector<float> sink(480 + 8, 0.0f);
    ring.writeSilence(target + 480);
    double consumerDebt = 0.0;
    netAdjust = 0;
    uint32_t minMarginLate = UINT32_MAX;
    for (int step = 0; step < 100 * 300; ++step) {  // 300 с
        const int d = fc.step();
        netAdjust += d;
        ring.write(frame.data(), uint32_t(480 + d));
        // consumer: 480 кадров минус накопившийся долг дрейфа
        consumerDebt += 480.0 * driftPpm * 1e-6;
        uint32_t want = 480;
        while (consumerDebt >= 1.0) {
            --want;
            consumerDebt -= 1.0;
        }
        while (consumerDebt <= -1.0) {
            ++want;
            consumerDebt += 1.0;
        }
        ring.read(sink.data(), want);
        fc.observe(ring.readable());
        if (step > 100 * 200) minMarginLate = std::min(minMarginLate, fc.marginFrames());
    }
    finalMargin = fc.marginFrames();
    (void)minMarginLate;
}

TEST_CASE("FillController converges under consumer drift", "[fill]") {
    const uint32_t target = 480;  // 10 ms
    uint32_t margin = 0;
    int64_t net = 0;

    SECTION("consumer slower (+140 ppm): samples must be dropped") {
        simulate(+140.0, target, margin, net);
        REQUIRE(net < 0);
        REQUIRE(margin < target + 480);  // без регулятора набежало бы 2 с
        REQUIRE(margin > target / 2);
    }
    SECTION("consumer faster (-140 ppm): samples must be inserted") {
        simulate(-140.0, target, margin, net);
        REQUIRE(net > 0);
        REQUIRE(margin > target / 2);
        REQUIRE(margin < target + 480);
    }
    SECTION("no drift: removes only the extra prefilled frame") {
        simulate(0.0, target, margin, net);
        REQUIRE(std::llabs(net + 480) < 100);
        REQUIRE(margin > target / 2);
        REQUIRE(margin < target + 480);
    }
}
