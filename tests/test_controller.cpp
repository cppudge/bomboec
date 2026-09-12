// Политика приложения (app/Controller) с поддельными движком и хостом: старт с backoff и
// уведомлением только о первой неудаче, поиск устройств по имени после смены id, кабель как
// выход по умолчанию, повтор reference, реакция на уведомления об устройствах, строка
// статуса раз в минуту.

#include "app/controller.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace bomboec;
using namespace bomboec::app;
namespace ws = bomboec::wasapi;
namespace fs = std::filesystem;

namespace {

struct FakeEngine final : IEngine {
    bool startOk = true;
    std::string startError = "device not found";
    EngineStatus st;
    int starts = 0, stops = 0, reopens = 0;
    bool reopenOk = false;
    AppConfig lastConfig;

    bool start(const AppConfig& cfg, std::string& error) override {
        ++starts;
        lastConfig = cfg;
        if (!startOk) {
            error = startError;
            return false;
        }
        st.running = true;
        // Как настоящий Engine: пустой id конфига раскрыт в устройство по умолчанию.
        st.micId = cfg.engine.micId.empty() ? "{yeti}" : cfg.engine.micId;
        st.outputId = cfg.engine.outputId.empty() ? "{cable-spk}" : cfg.engine.outputId;
        return true;
    }
    void stop() override {
        if (st.running) ++stops;
        st.running = false;
    }
    bool running() const override { return st.running; }
    EngineStatus status() const override { return st; }
    bool reopenReference(std::string& error) override {
        ++reopens;
        if (!reopenOk) {
            error = "no speakers";
            return false;
        }
        st.referenceActive = true;
        st.speakersName = "Speakers (Sound Blaster Z)";
        return true;
    }
};

struct FakeHost final : IHost {
    std::vector<std::string> logs, notifies;
    int statusShown = 0, stateChanges = 0;
    uint64_t now = 100'000;
    std::vector<ws::DeviceInfo> capture, render;

    void log(const std::string& line) override { logs.push_back(line); }
    void notify(const std::string& title, const std::string& text, Level) override {
        notifies.push_back(title + ": " + text);
    }
    void showStatus() override { ++statusShown; }
    std::vector<ws::DeviceInfo> devices(ws::Flow flow) override { return flow == ws::Flow::Capture ? capture : render; }
    uint64_t nowMs() override { return now; }
    void engineStateChanged() override { ++stateChanges; }

    bool logged(const std::string& part) const {
        return std::any_of(logs.begin(), logs.end(), [&](const std::string& l) { return l.find(part) != l.npos; });
    }
    bool notified(const std::string& part) const {
        return std::any_of(notifies.begin(), notifies.end(),
                           [&](const std::string& l) { return l.find(part) != l.npos; });
    }
};

struct Fixture {
    fs::path dir;
    FakeEngine engine;
    FakeHost host;
    Controller ctl;

    Fixture()
        : dir(fs::temp_directory_path() /
              ("bomboec-controller-test-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
          ctl(engine, host, dir / "bomboec.toml", dir / "bomboec.state.toml") {
        fs::create_directories(dir);
        host.capture = {{L"{yeti}", L"Microphone (Yeti Orb)", L"Yeti Orb", L"USB"},
                        {L"{cable-mic}", L"Microphone (bomboec Cable)", L"bomboec Cable", L"ROOT"}};
        host.render = {{L"{sb}", L"Speakers (Sound Blaster Z)", L"Sound Blaster Z", L"HDAUDIO"},
                       {L"{cable-spk}", L"Speakers (bomboec Cable)", L"bomboec Cable", L"ROOT"}};
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    // Перегрузка remove_all с error_code не бросает, но clang-tidy этого не знает.
    ~Fixture() noexcept {  // NOLINT(bugprone-exception-escape)
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // Секунда жизни: движок отдал кадры (иначе watchdog сочтёт микрофон зависшим), тик.
    void second() {
        host.now += 1000;
        if (engine.st.running) engine.st.framesProcessed += 100;
        ctl.tick();
    }
};

}  // namespace

TEST_CASE("Controller creates the config from the template and takes the cable as the output", "[controller]") {
    Fixture f;
    std::string error;
    REQUIRE(f.ctl.loadOrCreateConfig(error));
    CHECK(fs::exists(f.dir / "bomboec.toml"));
    CHECK(f.host.logged("config created"));
    CHECK(f.host.notifies.empty());  // шаблон без предупреждений

    f.ctl.start(true);
    CHECK(f.engine.starts == 1);
    CHECK(f.ctl.config().engine.outputId == "{cable-spk}");
    CHECK(f.host.logged("output defaulted to 'Speakers (bomboec Cable)'"));
    CHECK(fs::exists(f.dir / "bomboec.state.toml"));  // выбор сохранён
    CHECK(f.host.logged("engine started"));
    CHECK(f.host.stateChanges == 1);
}

TEST_CASE("Controller finds a device by name when its id changed", "[controller]") {
    Fixture f;
    std::ofstream(f.dir / "bomboec.toml") << "[devices]\nmic = \"{old-yeti}\"\nmic_name = \"Microphone (Yeti Orb)\"\n"
                                             "speakers = \"{gone}\"\nspeakers_name = \"Headphones\"\n";
    std::string error;
    REQUIRE(f.ctl.loadOrCreateConfig(error));
    f.ctl.start(true);
    CHECK(f.ctl.config().engine.micId == "{yeti}");
    CHECK(f.host.logged("mic: id not found, matched by name"));
    CHECK(f.ctl.config().engine.speakersId == "{gone}");  // имени нет в списке: остаётся как есть
    CHECK(f.host.logged("speakers: device 'Headphones' ({gone}) not present"));
}

TEST_CASE("Controller retries a failed start with backoff and notifies once", "[controller]") {
    Fixture f;
    std::string error;
    REQUIRE(f.ctl.loadOrCreateConfig(error));
    f.engine.startOk = false;
    f.ctl.start(false);
    CHECK(f.engine.starts == 1);
    CHECK(f.host.notified("start failed"));
    CHECK(f.ctl.lastError() == "device not found");
    CHECK(f.host.statusShown == 0);  // не по действию пользователя

    // 1 с backoff: следующая попытка через секунду, дальше уведомлений нет, только лог.
    f.ctl.tick();
    CHECK(f.engine.starts == 1);
    f.host.now += 1000;
    f.ctl.tick();
    CHECK(f.engine.starts == 2);
    CHECK(f.host.notifies.size() == 1);
    CHECK(f.host.logged("start failed"));

    // Устройство вернулось: старт удался, уведомление "running again".
    f.engine.startOk = true;
    f.host.now += 2000;
    f.ctl.tick();
    CHECK(f.engine.running());
    CHECK(f.host.notified("running again"));
    CHECK(f.ctl.lastError().empty());

    // Интерактивный неудачный старт открывает окно статуса.
    f.engine.startOk = false;
    f.ctl.reload();
    CHECK(f.host.statusShown == 1);
}

TEST_CASE("Controller restarts on a thread error and on a stalled microphone", "[controller]") {
    Fixture f;
    std::string error;
    REQUIRE(f.ctl.loadOrCreateConfig(error));
    f.ctl.start(true);
    f.engine.st.framesProcessed = 100;
    f.host.now += 1000;
    f.ctl.tick();
    CHECK(f.host.logged("audio running"));

    f.engine.st.error = "GetBuffer: AUDCLNT_E_DEVICE_INVALIDATED";
    f.host.now += 1000;
    f.ctl.tick();
    CHECK(f.engine.stops == 1);
    CHECK(f.host.notified("restarting"));
    CHECK(f.ctl.lastError() == "GetBuffer: AUDCLNT_E_DEVICE_INVALIDATED");
    f.engine.st.error.clear();
    f.host.now += 1000;
    f.ctl.tick();
    CHECK(f.engine.starts == 2);

    // Кадры не идут 3 с.
    for (int i = 0; i < 4; ++i) {
        f.host.now += 1000;
        f.ctl.tick();
    }
    CHECK(f.engine.stops == 2);
    CHECK(f.host.logged("no audio from the microphone for 3 s"));
}

TEST_CASE("Controller reopens the reference every 5 s until the speakers are back", "[controller]") {
    Fixture f;
    std::string error;
    REQUIRE(f.ctl.loadOrCreateConfig(error));
    f.ctl.start(true);
    f.engine.st.referenceActive = false;
    for (int i = 0; i < 4; ++i) f.second();
    CHECK(f.engine.reopens == 0);
    f.second();
    CHECK(f.engine.reopens == 1);
    CHECK(f.host.notified("no reference"));
    f.engine.reopenOk = true;
    for (int i = 0; i < 5; ++i) f.second();
    CHECK(f.engine.reopens == 2);
    CHECK(f.host.logged("reference back"));
    CHECK(f.host.notified("reference is back"));
}

TEST_CASE("Controller reacts to device notifications only when its devices are affected", "[controller]") {
    Fixture f;
    std::string error;
    REQUIRE(f.ctl.loadOrCreateConfig(error));
    f.ctl.start(true);
    f.engine.st.framesProcessed = 1;
    const int starts = f.engine.starts;

    f.ctl.onDevicesChanged(false);  // чужое устройство
    CHECK(f.engine.stops == 0);

    f.host.capture.erase(f.host.capture.begin());  // Yeti выдернули
    f.ctl.onDevicesChanged(false);
    CHECK(f.engine.stops == 1);
    CHECK(f.host.logged("microphone gone"));
    CHECK(f.engine.starts == starts + 1);  // попытка сразу, без backoff (не удалась: id нет, но движок поддельный)

    f.ctl.stop();
    f.ctl.onDevicesChanged(false);  // движок стоит, должен работать: старт сразу
    CHECK(f.host.logged("retrying start"));
}

TEST_CASE("Controller logs a status line once a minute and selects devices through the state file", "[controller]") {
    Fixture f;
    std::string error;
    REQUIRE(f.ctl.loadOrCreateConfig(error));
    f.ctl.start(true);
    for (int i = 0; i < 59; ++i) f.second();
    CHECK_FALSE(f.host.logged("status: "));
    f.second();
    CHECK(f.host.logged("status: mic"));

    f.ctl.selectDevice(Controller::Slot::Mic, f.host.capture[0]);
    CHECK(f.ctl.config().engine.micId == "{yeti}");
    CHECK(f.host.logged("device selected: 'Microphone (Yeti Orb)'"));
    CHECK(f.engine.stops == 1);
    CHECK(f.engine.running());
    EngineSettings saved;
    REQUIRE(loadState(f.dir / "bomboec.state.toml", saved, error));
    CHECK(saved.micId == "{yeti}");

    f.ctl.selectDevice(Controller::Slot::Speakers, std::nullopt);
    CHECK(f.ctl.config().engine.speakersId.empty());

    f.ctl.toggle();
    CHECK_FALSE(f.engine.running());
    CHECK_FALSE(f.ctl.wantRunning());
    f.ctl.tick();  // остановлен пользователем: watchdog не перезапускает
    CHECK_FALSE(f.engine.running());
}
