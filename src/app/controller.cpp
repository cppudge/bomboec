#include "app/controller.h"

#include "core/utf8.h"
#include "wasapi/com_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace bomboec::app {

namespace ws = wasapi;

Controller::Controller(IEngine& engine, IHost& host, std::filesystem::path configPath, std::filesystem::path statePath)
    : engine_(engine), host_(host), configPath_(std::move(configPath)), statePath_(std::move(statePath)) {}

bool Controller::loadOrCreateConfig(std::string& error) {
    if (!std::filesystem::exists(configPath_)) {
        if (!writeFileAtomic(configPath_, defaultConfigToml(), error)) return false;
        host_.log("config created: " + pathToUtf8(configPath_));
    }
    if (!loadConfig(configPath_, cfg_, error)) return false;
    // Устройства, выбранные в меню, поверх [devices] конфига.
    std::string stateError;
    if (!loadState(statePath_, cfg_.engine, stateError)) host_.log("state ignored: " + stateError);
    for (const std::string& w : cfg_.warnings) host_.log(w);
    if (!cfg_.warnings.empty()) {
        const size_t more = cfg_.warnings.size() - 1;
        host_.notify("bomboec: config warnings",
                     cfg_.warnings.front() + (more ? " (+" + std::to_string(more) + " in the log)" : ""),
                     IHost::Level::Warning);
    }
    return true;
}

void Controller::resolveDevicesByName() {
    const std::vector<ws::DeviceInfo> capDevices = host_.devices(ws::Flow::Capture);
    const std::vector<ws::DeviceInfo> renDevices = host_.devices(ws::Flow::Render);
    bool changed = false;
    auto fix = [&](std::string& id, const std::string& name, const std::vector<ws::DeviceInfo>& list,
                   const char* what) {
        if (id.empty()) return;
        const std::wstring wid = ws::fromUtf8(id);
        for (const ws::DeviceInfo& d : list) {
            if (d.id == wid) return;
        }
        const std::wstring wname = ws::fromUtf8(name);
        for (const ws::DeviceInfo& d : list) {
            if (!wname.empty() && d.name == wname) {
                host_.log(std::string(what) + ": id not found, matched by name '" + name + "'");
                id = ws::toUtf8(d.id);
                changed = true;
                return;
            }
        }
        host_.log(std::string(what) + ": device '" + name + "' (" + id + ") not present");
    };
    fix(cfg_.engine.micId, cfg_.engine.micName, capDevices, "mic");
    fix(cfg_.engine.speakersId, cfg_.engine.speakersName, renDevices, "speakers");
    fix(cfg_.engine.outputId, cfg_.engine.outputName, renDevices, "output");
    // Выход не выбран: если установлен наш кабель, берём его.
    if (cfg_.engine.outputId.empty()) {
        for (const ws::DeviceInfo& d : renDevices) {
            if (d.name.find(L"bomboec Cable") != std::wstring::npos) {
                cfg_.engine.outputId = ws::toUtf8(d.id);
                cfg_.engine.outputName = ws::toUtf8(d.name);
                host_.log("output defaulted to '" + cfg_.engine.outputName + "'");
                changed = true;
                break;
            }
        }
    }
    std::string error;
    if (changed && !saveState(statePath_, cfg_.engine, error)) host_.log("state not saved: " + error);
}

void Controller::start(bool interactive) {
    resolveDevicesByName();
    std::string error;
    const uint64_t now = host_.nowMs();
    if (!engine_.start(cfg_, error)) {
        lastError_ = error;
        watchdog_.onFailed(now);
        const std::string retry = " (retry in " + std::to_string((watchdog_.nextAttemptMs() - now) / 1000) + " s)";
        if (interactive || watchdog_.failures() == 1) {
            host_.notify("bomboec: start failed", error + retry, IHost::Level::Error);
        } else {
            host_.log("start failed: " + error + retry);
        }
        if (interactive) host_.showStatus();
    } else {
        if (watchdog_.failures() > 0)
            host_.notify("bomboec: running again", "audio devices are back", IHost::Level::Info);
        watchdog_.onStarted(now);
        audioLogged_ = false;
        lastError_.clear();
        refRetryAtMs_ = now + kRefRetryMs;
        refWarned_ = false;
        statusLogTicks_ = 0;
        const EngineStatus s = engine_.status();
        char line[1024];
        std::snprintf(line, sizeof(line),
                      "engine started: mic '%s' (raw %s, %u ch, %s), speakers '%s' (loopback %u ch, %s), "
                      "output '%s' (wasapi %u ms)",
                      s.micName.c_str(), s.micRaw ? "on" : "off", s.micDeviceChannels,
                      s.micEventDriven ? "event" : "polling", s.referenceActive ? s.speakersName.c_str() : "none",
                      s.refDeviceChannels, s.refEventDriven ? "event" : "polling", s.outputName.c_str(), s.outRenderMs);
        host_.log(line);
        if (!s.warning.empty()) {
            if (interactive) host_.notify("bomboec: check the config", s.warning, IHost::Level::Warning);
            else host_.log("warning: " + s.warning);
            refWarned_ = !s.referenceActive;
        }
    }
    host_.engineStateChanged();
}

void Controller::stop() {
    const bool wasRunning = engine_.running();
    engine_.stop();  // присоединяет потоки: строка в логе только после возврата
    if (wasRunning) host_.log("engine stopped");
    host_.engineStateChanged();
}

void Controller::restartIfRunning() {
    if (engine_.running()) stop();
    watchdog_.clear();
    if (wantRunning_) start(true);
}

void Controller::toggle() {
    wantRunning_ = !engine_.running();
    watchdog_.clear();
    if (wantRunning_) start(true);
    else stop();
}

void Controller::reload() {
    std::string error;
    if (!loadOrCreateConfig(error)) {
        host_.notify("bomboec: config error", error, IHost::Level::Error);
    } else {
        restartIfRunning();
    }
}

void Controller::selectDevice(Slot slot, const std::optional<ws::DeviceInfo>& device) {
    std::string* id = &cfg_.engine.micId;
    std::string* name = &cfg_.engine.micName;
    if (slot == Slot::Speakers) {
        id = &cfg_.engine.speakersId;
        name = &cfg_.engine.speakersName;
    } else if (slot == Slot::Output) {
        id = &cfg_.engine.outputId;
        name = &cfg_.engine.outputName;
    }
    if (device) {
        *id = ws::toUtf8(device->id);
        *name = ws::toUtf8(device->name);
    } else {
        id->clear();
        name->clear();
    }
    std::string error;
    if (!saveState(statePath_, cfg_.engine, error)) host_.notify("bomboec", error, IHost::Level::Warning);
    host_.log("device selected: '" + *name + "' (" + *id + ")");
    restartIfRunning();
}

void Controller::disable(const std::string& error) {
    wantRunning_ = false;
    lastError_ = error;
}

// Reference (loopback колонок) необязателен: без него движок работает, но эхо не
// подавляется. Пока его нет, раз в kRefRetryMs пробуем открыть заново (колонки вернулись).
void Controller::retryReference(const EngineStatus& s, uint64_t now) {
    if (!s.running || s.referenceActive || now < refRetryAtMs_) return;
    refRetryAtMs_ = now + kRefRetryMs;
    std::string error;
    if (engine_.reopenReference(error)) {
        const EngineStatus after = engine_.status();
        host_.log("reference back: '" + after.speakersName + "'");
        if (refWarned_) {
            host_.notify("bomboec: reference is back", "echo cancellation resumed on '" + after.speakersName + "'",
                         IHost::Level::Info);
        }
        refWarned_ = false;
    } else if (!refWarned_) {
        refWarned_ = true;
        host_.notify("bomboec: no reference", error, IHost::Level::Warning);
    }
}

std::string Controller::statusLine(const EngineStatus& s) {
    auto num = [](const std::optional<double>& v) {
        return v ? std::to_string(int(std::lround(*v))) : std::string("-");
    };
    char line[512];
    std::snprintf(line, sizeof(line),
                  "status: mic %.0f ref %.0f out %.0f dBFS, aec delay %s erle %s, ref %s missing %llu jumps %llu "
                  "gaps %llu/%llu resyncs %llu/%llu, out %u ms margin %u under %llu over %llu fill +%llu/-%llu "
                  "trim %llu, drift %+.0f/%+.0f ppm, frames %llu%s",
                  s.micDb, s.refDb, s.outDb, num(s.stats.delayMs).c_str(), num(s.stats.erleDb).c_str(),
                  s.referenceActive ? "on" : "off", (unsigned long long)s.refMissing, (unsigned long long)s.refJumps,
                  (unsigned long long)s.micGaps, (unsigned long long)s.refGaps, (unsigned long long)s.micResyncs,
                  (unsigned long long)s.refResyncs, s.outBufferedMs, s.outMarginMs, (unsigned long long)s.outUnderruns,
                  (unsigned long long)s.outOverruns, (unsigned long long)s.outInserted,
                  (unsigned long long)s.outDropped, (unsigned long long)s.outTrimmed, s.micDriftPpm, s.refDriftPpm,
                  (unsigned long long)s.framesProcessed, s.warning.empty() ? "" : (", warning: " + s.warning).c_str());
    return line;
}

// Раз в секунду: решения Watchdog (перезапуск после ошибки потока или зависания, повтор
// неудачного старта с backoff), повтор reference, раз в минуту строка статуса в лог.
void Controller::tick() {
    if (!wantRunning_) return;
    const uint64_t now = host_.nowMs();
    const EngineStatus s = engine_.status();
    if (s.running && !audioLogged_ && s.framesProcessed > 0) {
        audioLogged_ = true;
        host_.log(std::string("audio running: mmcss ") + (s.mmcss ? "on" : "off"));
    }
    if (s.running && ++statusLogTicks_ >= kStatusLogEverySec) {
        statusLogTicks_ = 0;
        host_.log(statusLine(s));
    }
    retryReference(s, now);
    switch (watchdog_.tick(now, s.running, !s.error.empty(), s.framesProcessed)) {
        case Watchdog::Action::Restart: {
            const std::string why =
                watchdog_.reason() == Watchdog::Reason::Stall ? "no audio from the microphone for 3 s" : s.error;
            lastError_ = why;
            stop();
            watchdog_.onFailed(now);
            if (watchdog_.failures() == 1) host_.notify("bomboec: restarting", why, IHost::Level::Warning);
            else host_.log("watchdog: " + why);
            break;
        }
        case Watchdog::Action::Start: start(false); break;
        case Watchdog::Action::None: break;
    }
}

// Устройство движка пропало или сменился default, который он использует, -> стоп и
// немедленный старт заново (без backoff); движок стоит -> попытка старта сразу; работает
// без reference -> открыть колонки сейчас. Чужие устройства движок не трогают.
void Controller::onDevicesChanged(bool defaultChanged) {
    const EngineStatus s = engine_.status();
    DeviceChangeFacts f;
    f.running = s.running;
    f.wantRunning = wantRunning_;
    f.referenceActive = s.referenceActive;
    f.defaultChanged = defaultChanged;
    f.usesDefault = cfg_.engine.micId.empty() || cfg_.engine.outputId.empty() || cfg_.engine.speakersId.empty();
    if (s.running) {
        const auto present = [](const std::vector<ws::DeviceInfo>& list, const std::string& id) {
            const std::wstring wid = ws::fromUtf8(id);
            return std::any_of(list.begin(), list.end(), [&](const ws::DeviceInfo& d) { return d.id == wid; });
        };
        f.micPresent = present(host_.devices(ws::Flow::Capture), s.micId);
        f.outputPresent = present(host_.devices(ws::Flow::Render), s.outputId);
    }
    const DeviceChangeAction action = decideDeviceChange(f);
    switch (action) {
        case DeviceChangeAction::Restart:
            host_.log(std::string("devices changed: ") +
                      (!f.micPresent      ? "microphone gone"
                       : !f.outputPresent ? "output gone"
                                          : "default changed") +
                      ", restarting");
            stop();
            watchdog_.retryNow();
            break;
        case DeviceChangeAction::ReopenReference: refRetryAtMs_ = 0; break;
        case DeviceChangeAction::StartNow:
            host_.log("devices changed: retrying start");
            watchdog_.retryNow();
            break;
        case DeviceChangeAction::None: break;
    }
    if (action != DeviceChangeAction::None) tick();
}

}  // namespace bomboec::app
