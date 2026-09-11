#include "core/timeline.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using bomboec::Timeline;
using Catch::Approx;

TEST_CASE("Timeline uses nominal rate until enough span, then measures drift") {
    // Устройство реально работает на 48000 * (1 + 100 ppm).
    const double tps = 10'000'000.0;  // 100 ns тики
    const double realRate = 48000.0 * 1.0001;
    Timeline tl(48000.0, tps);
    tl.setMinRateSpanSeconds(2.0);
    REQUIRE_FALSE(tl.valid());

    // Якорь каждые 10 ms по реальным часам устройства.
    uint64_t sample = 0;
    for (int i = 0; i <= 500; ++i) {  // 5 секунд
        const double t = sample / realRate;
        tl.anchor(int64_t(t * tps), sample);
        if (i == 100) {
            // 1 с < 2 с: ещё номинал.
            CHECK(tl.estimatedRate() == Approx(48000.0));
        }
        sample += 480;
    }
    CHECK(tl.valid());
    CHECK(tl.driftPpm() == Approx(100.0).margin(1.0));

    // Отображение времени в сэмплы и обратно вокруг последнего якоря.
    const int64_t lastTicks = tl.lastTicks();
    CHECK(tl.sampleAt(lastTicks) == Approx(double(tl.lastSample())));
    const int64_t plus10ms = lastTicks + int64_t(0.010 * tps);
    CHECK(tl.sampleAt(plus10ms) == Approx(double(tl.lastSample()) + 480.048).margin(0.01));
    CHECK(tl.ticksAt(double(tl.lastSample()) + 480.048) == Approx(double(plus10ms)).margin(2.0));
}
