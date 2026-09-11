#include "engine/watchdog.h"

#include <catch2/catch_test_macros.hpp>

using bomboec::Watchdog;
using Action = Watchdog::Action;
using Reason = Watchdog::Reason;

TEST_CASE("Watchdog restarts after a thread error and after a stall", "[watchdog]") {
    Watchdog w;
    w.onStarted(0);
    CHECK(w.tick(1000, true, false, 100) == Action::None);
    CHECK(w.tick(2000, true, false, 200) == Action::None);
    // Кадры перестали идти: ошибки нет, но микрофон молчит.
    CHECK(w.tick(3000, true, false, 200) == Action::None);
    CHECK(w.tick(4000, true, false, 200) == Action::None);
    CHECK(w.tick(5000, true, false, 200) == Action::Restart);
    CHECK(w.reason() == Reason::Stall);

    Watchdog e;
    e.onStarted(0);
    CHECK(e.tick(1000, true, true, 50) == Action::Restart);
    CHECK(e.reason() == Reason::ThreadError);
}

TEST_CASE("Watchdog retries a failing start with growing backoff", "[watchdog]") {
    Watchdog w;
    uint64_t now = 0;
    CHECK(w.tick(now, false, false, 0) == Action::Start);  // первая попытка сразу

    for (const uint64_t delay : {1000, 2000, 5000, 10'000, 30'000, 30'000}) {
        w.onFailed(now);
        CHECK(w.tick(now + delay - 1, false, false, 0) == Action::None);
        now += delay;
        CHECK(w.tick(now, false, false, 0) == Action::Start);
    }
    CHECK(w.failures() == 6);

    // Старт удался: неудачи забываются только после 30 с нормальной работы.
    w.onStarted(now);
    CHECK(w.tick(now + 1000, true, false, 100) == Action::None);
    CHECK(w.failures() == 6);
    CHECK(w.tick(now + 31'000, true, false, 3100) == Action::None);
    CHECK(w.failures() == 0);
    w.onFailed(now + 32'000);
    CHECK(w.nextAttemptMs() == now + 33'000);

    w.clear();
    CHECK(w.failures() == 0);
    CHECK(w.tick(now + 32'001, false, false, 0) == Action::Start);
}

TEST_CASE("Device notifications: restart only when the engine's devices are affected", "[watchdog]") {
    using bomboec::decideDeviceChange;
    using bomboec::DeviceChangeAction;
    using bomboec::DeviceChangeFacts;
    DeviceChangeFacts f;
    f.running = true;
    CHECK(decideDeviceChange(f) == DeviceChangeAction::None);  // чужое устройство: ничего

    f.micPresent = false;
    CHECK(decideDeviceChange(f) == DeviceChangeAction::Restart);
    f.micPresent = true;
    f.outputPresent = false;
    CHECK(decideDeviceChange(f) == DeviceChangeAction::Restart);
    f.outputPresent = true;

    f.defaultChanged = true;
    CHECK(decideDeviceChange(f) == DeviceChangeAction::None);  // устройства заданы явно
    f.usesDefault = true;
    CHECK(decideDeviceChange(f) == DeviceChangeAction::Restart);
    f.defaultChanged = false;

    f.referenceActive = false;
    CHECK(decideDeviceChange(f) == DeviceChangeAction::ReopenReference);

    f.running = false;
    CHECK(decideDeviceChange(f) == DeviceChangeAction::StartNow);  // устройство вернулось: без backoff
    f.wantRunning = false;
    CHECK(decideDeviceChange(f) == DeviceChangeAction::None);
}
