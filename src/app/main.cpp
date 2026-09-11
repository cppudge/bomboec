// bomboec: tray-приложение вокруг Engine.
//
// Конфиг: bomboec.toml рядом с exe (создаётся из config/default.toml, встроенного при сборке).
// Лог: bomboec.log рядом с exe. Один экземпляр: повторный запуск показывает
// окно статуса уже работающего.
// Меню в трее: старт/стоп, выбор микрофона, колонок (reference) и выхода,
// окно статуса, открыть конфиг, выход. Выбор устройства сохраняется в
// bomboec.state.toml (id и имя; конфиг приложение не переписывает) и
// перезапускает движок. Если id устройства больше нет (переустановка драйвера
// кабеля), устройство ищется по имени.
//
// Окно приложения скрытое top-level, а не message-only: только такое получает
// TaskbarCreated (иконка возвращается после перезапуска explorer.exe) и находится
// через FindWindow из второго экземпляра.

#include "app/crash_dump.h"
#include "core/config.h"
#include "core/utf8.h"
#include "engine/engine.h"
#include "engine/watchdog.h"
#include "version.h"
#include "wasapi/com_util.h"
#include "wasapi/device_notifier.h"
#include "wasapi/devices.h"

#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace bomboec;
namespace ws = bomboec::wasapi;

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_SHOW_STATUS = WM_APP + 2;
constexpr UINT WM_DEVICE_CHANGE = WM_APP + 3;  // из DeviceNotifier (MTA-поток): wp = DeviceNotifier::Event
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kWatchdogTimer = 2;
constexpr UINT_PTR kDeviceTimer = 3;  // дебаунс уведомлений об устройствах
constexpr UINT kDeviceDebounceMs = 500;
constexpr uintmax_t kLogRotateBytes = 1 << 20;  // bomboec.log -> bomboec.log.1
constexpr UINT ID_TOGGLE = 100, ID_STATUS = 101, ID_CONFIG = 102, ID_RELOAD = 103, ID_EXIT = 104, ID_LOG = 105;
constexpr UINT ID_MIC_BASE = 1000, ID_SPK_BASE = 2000, ID_OUT_BASE = 3000;
const wchar_t* kTrayClass = L"BomboecTray";
const wchar_t* kMutexName = L"Local\\bomboec.tray.single";

struct App {
    HWND hwnd = nullptr;
    HWND statusWnd = nullptr;
    HWND statusEdit = nullptr;
    NOTIFYICONDATAW nid{};
    bool trayAdded = false;
    UINT taskbarCreatedMsg = 0;  // RegisterWindowMessage(L"TaskbarCreated")
    HFONT statusFont = nullptr;
    std::filesystem::path configPath, statePath, logPath;
    AppConfig cfg;
    Engine engine;
    Watchdog watchdog;
    ws::DeviceNotifier notifier;
    bool defaultChangedPending = false;  // среди собранных уведомлений была смена default
    bool audioLogged = false;            // после старта в лог уже записано, что звук пошёл
    // Списки устройств для открытого меню: пункты меню индексируют именно их. Watchdog
    // за время, пока меню открыто, может перечислить устройства заново, поэтому у него
    // свои списки (resolveDevicesByName).
    std::vector<ws::DeviceInfo> menuCapDevices, menuRenDevices;
    std::string lastError;
    bool wantRunning = true;
    uint64_t refRetryAtMs = 0;  // когда снова пробовать открыть reference (loopback колонок)
    bool refWarned = false;     // об отсутствии reference уже сообщили
};

constexpr uint64_t kRefRetryMs = 5000;

App* gApp = nullptr;

std::filesystem::path exeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

void logLine(App& app, const std::string& text) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(app.logPath, ec);
    if (!ec && size > kLogRotateBytes) {
        std::filesystem::path old = app.logPath;
        old += ".1";
        std::filesystem::rename(app.logPath, old, ec);  // заменяет прежний .1
    }
    std::ofstream out(app.logPath, std::ios::app | std::ios::binary);
    if (!out) return;
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    out << stamp << "  " << text << "\n";
}

void notify(App& app, const wchar_t* title, const std::string& text, DWORD flags = NIIF_INFO) {
    logLine(app, ws::toUtf8(title) + ": " + text);
    NOTIFYICONDATAW n = app.nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = flags;
    wcsncpy_s(n.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(n.szInfo, ws::fromUtf8(text).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void updateTooltip(App& app) {
    NOTIFYICONDATAW n = app.nid;
    n.uFlags = NIF_TIP;
    const std::wstring tip = app.engine.running() ? L"bomboec: running" : L"bomboec: stopped";
    wcsncpy_s(n.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// Иконка в трее. Повторяется по TaskbarCreated (перезапуск explorer.exe) и из таймера
// watchdog, пока оболочка не готова (автозапуск раньше панели задач).
void addTrayIcon(App& app) {
    app.trayAdded = Shell_NotifyIconW(NIM_ADD, &app.nid) || Shell_NotifyIconW(NIM_MODIFY, &app.nid);
    if (!app.trayAdded) return;
    // Сообщения трея в формате NOTIFYICON_VERSION_4 (см. WM_TRAY в WndProc).
    Shell_NotifyIconW(NIM_SETVERSION, &app.nid);
    updateTooltip(app);
}

bool loadOrCreateConfig(App& app, std::string& error) {
    if (!std::filesystem::exists(app.configPath)) {
        if (!writeFileAtomic(app.configPath, defaultConfigToml(), error)) return false;
        logLine(app, "config created: " + pathToUtf8(app.configPath));
    }
    if (!loadConfig(app.configPath, app.cfg, error)) return false;
    // Устройства, выбранные в меню, поверх [devices] конфига.
    std::string stateError;
    if (!loadState(app.statePath, app.cfg.engine, stateError)) logLine(app, "state ignored: " + stateError);
    for (const std::string& w : app.cfg.warnings) logLine(app, w);
    if (!app.cfg.warnings.empty()) {
        const size_t more = app.cfg.warnings.size() - 1;
        notify(app, L"bomboec: config warnings",
               app.cfg.warnings.front() + (more ? " (+" + std::to_string(more) + " in the log)" : ""), NIIF_WARNING);
    }
    return true;
}

// Если сохранённого id нет среди устройств, ищем по имени и обновляем id.
void resolveDevicesByName(App& app) {
    std::string error;
    const std::vector<ws::DeviceInfo> capDevices = ws::enumerateDevices(ws::Flow::Capture, error);
    const std::vector<ws::DeviceInfo> renDevices = ws::enumerateDevices(ws::Flow::Render, error);
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
                logLine(app, std::string(what) + ": id not found, matched by name '" + name + "'");
                id = ws::toUtf8(d.id);
                changed = true;
                return;
            }
        }
        logLine(app, std::string(what) + ": device '" + name + "' (" + id + ") not present");
    };
    fix(app.cfg.engine.micId, app.cfg.engine.micName, capDevices, "mic");
    fix(app.cfg.engine.speakersId, app.cfg.engine.speakersName, renDevices, "speakers");
    fix(app.cfg.engine.outputId, app.cfg.engine.outputName, renDevices, "output");
    // Выход не выбран: если установлен наш кабель, берём его.
    if (app.cfg.engine.outputId.empty()) {
        for (const ws::DeviceInfo& d : renDevices) {
            if (d.name.find(L"bomboec Cable") != std::wstring::npos) {
                app.cfg.engine.outputId = ws::toUtf8(d.id);
                app.cfg.engine.outputName = ws::toUtf8(d.name);
                logLine(app, "output defaulted to '" + app.cfg.engine.outputName + "'");
                changed = true;
                break;
            }
        }
    }
    if (changed && !saveState(app.statePath, app.cfg.engine, error)) logLine(app, "state not saved: " + error);
}

void showStatusWindow(App& app);

// interactive: запуск по действию пользователя (при ошибке открывается окно статуса). Иначе
// это повтор watchdog'а: уведомление только о первой неудаче серии, остальные в лог.
void startEngine(App& app, bool interactive) {
    resolveDevicesByName(app);
    std::string error;
    const uint64_t now = GetTickCount64();
    if (!app.engine.start(app.cfg, error)) {
        app.lastError = error;
        app.watchdog.onFailed(now);
        const std::string retry = " (retry in " + std::to_string((app.watchdog.nextAttemptMs() - now) / 1000) + " s)";
        if (interactive || app.watchdog.failures() == 1) {
            notify(app, L"bomboec: start failed", error + retry, NIIF_ERROR);
        } else {
            logLine(app, "start failed: " + error + retry);
        }
        if (interactive) showStatusWindow(app);
    } else {
        if (app.watchdog.failures() > 0) notify(app, L"bomboec: running again", "audio devices are back");
        app.watchdog.onStarted(now);
        app.audioLogged = false;
        app.lastError.clear();
        app.refRetryAtMs = now + kRefRetryMs;
        app.refWarned = false;
        const EngineStatus s = app.engine.status();
        char line[1024];
        std::snprintf(line, sizeof(line),
                      "engine started: mic '%s' (raw %s, %u ch, %s), speakers '%s' (loopback %u ch, %s), "
                      "output '%s' (wasapi %u ms)",
                      s.micName.c_str(), s.micRaw ? "on" : "off", s.micDeviceChannels,
                      s.micEventDriven ? "event" : "polling", s.referenceActive ? s.speakersName.c_str() : "none",
                      s.refDeviceChannels, s.refEventDriven ? "event" : "polling", s.outputName.c_str(), s.outRenderMs);
        logLine(app, line);
        if (!s.warning.empty()) {
            if (interactive) notify(app, L"bomboec: check the config", s.warning, NIIF_WARNING);
            else logLine(app, "warning: " + s.warning);
            app.refWarned = !s.referenceActive;
        }
    }
    updateTooltip(app);
}

void stopEngine(App& app) {
    const bool wasRunning = app.engine.running();
    app.engine.stop();  // присоединяет потоки: строка в логе только после возврата
    if (wasRunning) logLine(app, "engine stopped");
    updateTooltip(app);
}

void restartIfRunning(App& app) {
    if (app.engine.running()) stopEngine(app);
    app.watchdog.clear();
    if (app.wantRunning) startEngine(app, true);
}

std::string statusText(App& app) {
    const EngineStatus s = app.engine.status();
    char buf[2048];
    if (!s.running) {
        std::snprintf(buf, sizeof(buf),
                      "bomboec %s: stopped\r\n\r\n%s\r\n\r\n"
                      "Правый клик по иконке в трее: выбор микрофона, колонок и выхода, старт.\r\n"
                      "Конфиг: %s\r\nЛог: %s",
                      BOMBOEC_VERSION_FULL, app.lastError.c_str(), pathToUtf8(app.configPath).c_str(),
                      pathToUtf8(app.logPath).c_str());
        return buf;
    }
    auto opt = [](const std::optional<double>& v, const char* unit) {
        char b[32];
        if (!v) return std::string("n/a");
        std::snprintf(b, sizeof(b), "%.1f %s", *v, unit);
        return std::string(b);
    };
    const std::string reference = s.referenceActive
                                      ? s.speakersName + " (" + (s.refEventDriven ? "event" : "polling") + ")"
                                      : std::string("none: echo is not cancelled, retrying");
    std::snprintf(
        buf, sizeof(buf),
        "bomboec %s: running\r\nmic:       %s (raw %s, %s)\r\nreference: %s\r\noutput:    %s\r\n"
        "threads:   mmcss %s\r\n\r\n"
        "levels     mic %6.1f   ref %6.1f   out %6.1f dBFS\r\n"
        "aec        delay %s   erl %s   erle %s   errors %llu   vad %s\r\n"
        "reference  lead %.0f ms   missing %llu   jumps %llu   gaps mic %llu / ref %llu   "
        "resyncs mic %llu / ref %llu\r\n"
        "output     ring %u ms (margin %u ms)   wasapi %u ms   underruns %llu   overruns %llu\r\n"
        "fill ctl   inserted %llu   dropped %llu   trimmed %llu samples\r\n"
        "drift      mic %+.0f ppm   ref %+.0f ppm\r\n"
        "frames     %llu\r\n%s%s",
        BOMBOEC_VERSION_FULL, s.micName.c_str(), s.micRaw ? "on" : "off", s.micEventDriven ? "event" : "polling",
        reference.c_str(), s.outputName.c_str(), s.mmcss ? "on" : "off", s.micDb, s.refDb, s.outDb,
        opt(s.stats.delayMs, "ms").c_str(), opt(s.stats.erlDb, "dB").c_str(), opt(s.stats.erleDb, "dB").c_str(),
        (unsigned long long)s.stats.errors, opt(s.stats.vadProbability, "").c_str(), s.referenceLeadMs,
        (unsigned long long)s.refMissing, (unsigned long long)s.refJumps, (unsigned long long)s.micGaps,
        (unsigned long long)s.refGaps, (unsigned long long)s.micResyncs, (unsigned long long)s.refResyncs,
        s.outBufferedMs, s.outMarginMs, s.outRenderMs, (unsigned long long)s.outUnderruns,
        (unsigned long long)s.outOverruns, (unsigned long long)s.outInserted, (unsigned long long)s.outDropped,
        (unsigned long long)s.outTrimmed, s.micDriftPpm, s.refDriftPpm, (unsigned long long)s.framesProcessed,
        s.warning.empty() ? "" : ("WARNING: " + s.warning + "\r\n").c_str(),
        s.error.empty() ? "" : ("ERROR: " + s.error).c_str());
    return buf;
}

LRESULT CALLBACK StatusWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App& app = *gApp;
    switch (msg) {
        case WM_SIZE:
            if (app.statusEdit) MoveWindow(app.statusEdit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
            return 0;
        case WM_TIMER:
            if (app.statusEdit) SetWindowTextW(app.statusEdit, ws::fromUtf8(statusText(app)).c_str());
            return 0;
        case WM_CLOSE:
            KillTimer(h, kStatusTimer);
            DestroyWindow(h);
            app.statusWnd = nullptr;
            app.statusEdit = nullptr;
            return 0;
        default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void showStatusWindow(App& app) {
    if (app.statusWnd) {
        ShowWindow(app.statusWnd, SW_SHOWNORMAL);
        SetForegroundWindow(app.statusWnd);
        return;
    }
    app.statusWnd = CreateWindowExW(0, L"BomboecStatus", L"bomboec status", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                    CW_USEDEFAULT, 700, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    app.statusEdit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                                     0, 0, 700, 300, app.statusWnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    // Шрифт один на всё время жизни приложения: окно открывается и закрывается многократно.
    if (!app.statusFont) {
        app.statusFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    }
    SendMessageW(app.statusEdit, WM_SETFONT, reinterpret_cast<WPARAM>(app.statusFont), TRUE);
    SetWindowTextW(app.statusEdit, ws::fromUtf8(statusText(app)).c_str());
    SetTimer(app.statusWnd, kStatusTimer, 500, nullptr);
    ShowWindow(app.statusWnd, SW_SHOWNORMAL);
    SetForegroundWindow(app.statusWnd);
}

void appendDeviceMenu(HMENU parent, const wchar_t* title, const std::vector<ws::DeviceInfo>& devices,
                      const std::string& selectedId, UINT baseId) {
    HMENU sub = CreatePopupMenu();
    const std::wstring sel = ws::fromUtf8(selectedId);
    AppendMenuW(sub, MF_STRING | (sel.empty() ? MF_CHECKED : 0), baseId, L"System default");
    for (size_t i = 0; i < devices.size(); ++i) {
        const bool checked = !sel.empty() && devices[i].id == sel;
        std::wstring name = devices[i].name;
        if (devices[i].isDefault) name += L"  (default)";
        AppendMenuW(sub, MF_STRING | (checked ? MF_CHECKED : 0), baseId + 1 + UINT(i), name.c_str());
    }
    AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), title);
}

void showMenu(App& app, int x, int y) {
    std::string error;
    app.menuCapDevices = ws::enumerateDevices(ws::Flow::Capture, error);
    app.menuRenDevices = ws::enumerateDevices(ws::Flow::Render, error);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (app.engine.running() ? MF_CHECKED : 0), ID_TOGGLE,
                app.engine.running() ? L"Running (click to stop)" : L"Start");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    appendDeviceMenu(menu, L"Microphone", app.menuCapDevices, app.cfg.engine.micId, ID_MIC_BASE);
    appendDeviceMenu(menu, L"Speakers (reference)", app.menuRenDevices, app.cfg.engine.speakersId, ID_SPK_BASE);
    appendDeviceMenu(menu, L"Output (virtual mic)", app.menuRenDevices, app.cfg.engine.outputId, ID_OUT_BASE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_STATUS, L"Status...");
    AppendMenuW(menu, MF_STRING, ID_CONFIG, L"Open config");
    AppendMenuW(menu, MF_STRING, ID_LOG, L"Open log");
    AppendMenuW(menu, MF_STRING, ID_RELOAD, L"Reload config && restart");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    SetForegroundWindow(app.hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, 0, app.hwnd, nullptr);
    PostMessageW(app.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void selectDevice(App& app, std::string& id, std::string& name, const std::vector<ws::DeviceInfo>& devices,
                  UINT index) {
    if (index == 0 || index - 1 >= devices.size()) {
        id.clear();
        name.clear();
    } else {
        id = ws::toUtf8(devices[index - 1].id);
        name = ws::toUtf8(devices[index - 1].name);
    }
    std::string error;
    if (!saveState(app.statePath, app.cfg.engine, error)) notify(app, L"bomboec", error, NIIF_WARNING);
    logLine(app, "device selected: '" + name + "' (" + id + ")");
    restartIfRunning(app);
}

void handleCommand(App& app, UINT id) {
    if (id == ID_TOGGLE) {
        app.wantRunning = !app.engine.running();
        app.watchdog.clear();
        if (app.wantRunning) startEngine(app, true);
        else stopEngine(app);
    } else if (id == ID_STATUS) {
        showStatusWindow(app);
    } else if (id == ID_CONFIG) {
        ShellExecuteW(nullptr, L"open", app.configPath.c_str(), nullptr, nullptr, SW_SHOW);
    } else if (id == ID_LOG) {
        ShellExecuteW(nullptr, L"open", app.logPath.c_str(), nullptr, nullptr, SW_SHOW);
    } else if (id == ID_RELOAD) {
        std::string error;
        if (!loadOrCreateConfig(app, error)) {
            notify(app, L"bomboec: config error", error, NIIF_ERROR);
        } else {
            restartIfRunning(app);
        }
    } else if (id == ID_EXIT) {
        DestroyWindow(app.hwnd);
    } else if (id >= ID_MIC_BASE && id < ID_MIC_BASE + 1000) {
        selectDevice(app, app.cfg.engine.micId, app.cfg.engine.micName, app.menuCapDevices, id - ID_MIC_BASE);
    } else if (id >= ID_SPK_BASE && id < ID_SPK_BASE + 1000) {
        selectDevice(app, app.cfg.engine.speakersId, app.cfg.engine.speakersName, app.menuRenDevices, id - ID_SPK_BASE);
    } else if (id >= ID_OUT_BASE && id < ID_OUT_BASE + 1000) {
        selectDevice(app, app.cfg.engine.outputId, app.cfg.engine.outputName, app.menuRenDevices, id - ID_OUT_BASE);
    }
}

// Reference (loopback колонок) необязателен: без него движок работает, но эхо не
// подавляется. Пока его нет, раз в kRefRetryMs пробуем открыть заново (колонки вернулись).
void retryReference(App& app, const EngineStatus& s, uint64_t now) {
    if (!s.running || s.referenceActive || now < app.refRetryAtMs) return;
    app.refRetryAtMs = now + kRefRetryMs;
    std::string error;
    if (app.engine.reopenReference(error)) {
        const EngineStatus after = app.engine.status();
        logLine(app, "reference back: '" + after.speakersName + "'");
        if (app.refWarned)
            notify(app, L"bomboec: reference is back", "echo cancellation resumed on '" + after.speakersName + "'");
        app.refWarned = false;
    } else if (!app.refWarned) {
        app.refWarned = true;
        notify(app, L"bomboec: no reference", error, NIIF_WARNING);
    }
}

// Раз в секунду: иконка трея, если оболочка была не готова, и решения Watchdog
// (перезапуск после ошибки потока или зависания, повтор неудачного старта с backoff).
void watchdogTick(App& app) {
    if (!app.trayAdded) addTrayIcon(app);
    if (!app.wantRunning) return;
    const uint64_t now = GetTickCount64();
    const EngineStatus s = app.engine.status();
    if (s.running && !app.audioLogged && s.framesProcessed > 0) {
        app.audioLogged = true;
        logLine(app, std::string("audio running: mmcss ") + (s.mmcss ? "on" : "off"));
    }
    retryReference(app, s, now);
    switch (app.watchdog.tick(now, s.running, !s.error.empty(), s.framesProcessed)) {
        case Watchdog::Action::Restart: {
            const std::string why =
                app.watchdog.reason() == Watchdog::Reason::Stall ? "no audio from the microphone for 3 s" : s.error;
            app.lastError = why;
            stopEngine(app);
            app.watchdog.onFailed(now);
            if (app.watchdog.failures() == 1) notify(app, L"bomboec: restarting", why, NIIF_WARNING);
            else logLine(app, "watchdog: " + why);
            break;
        }
        case Watchdog::Action::Start: startEngine(app, false); break;
        case Watchdog::Action::None: break;
    }
}

// Уведомления об устройствах собраны за окно дебаунса: устройство движка пропало или
// сменился default, который он использует, -> стоп и немедленный старт заново (watchdog
// без backoff); движок стоит -> попытка старта сразу; работает без reference -> открыть
// колонки сейчас. Чужие устройства движок не трогают.
void onDevicesChanged(App& app) {
    const bool defaultChanged = app.defaultChangedPending;
    app.defaultChangedPending = false;
    const EngineStatus s = app.engine.status();
    DeviceChangeFacts f;
    f.running = s.running;
    f.wantRunning = app.wantRunning;
    f.referenceActive = s.referenceActive;
    f.defaultChanged = defaultChanged;
    f.usesDefault =
        app.cfg.engine.micId.empty() || app.cfg.engine.outputId.empty() || app.cfg.engine.speakersId.empty();
    if (s.running) {
        std::string error;
        const auto present = [](const std::vector<ws::DeviceInfo>& list, const std::string& id) {
            const std::wstring wid = ws::fromUtf8(id);
            return std::any_of(list.begin(), list.end(), [&](const ws::DeviceInfo& d) { return d.id == wid; });
        };
        f.micPresent = present(ws::enumerateDevices(ws::Flow::Capture, error), s.micId);
        f.outputPresent = present(ws::enumerateDevices(ws::Flow::Render, error), s.outputId);
    }
    switch (decideDeviceChange(f)) {
        case DeviceChangeAction::Restart:
            logLine(app, std::string("devices changed: ") +
                             (!f.micPresent      ? "microphone gone"
                              : !f.outputPresent ? "output gone"
                                                 : "default changed") +
                             ", restarting");
            stopEngine(app);
            app.watchdog.retryNow();
            break;
        case DeviceChangeAction::ReopenReference: app.refRetryAtMs = 0; break;
        case DeviceChangeAction::StartNow:
            logLine(app, "devices changed: retrying start");
            app.watchdog.retryNow();
            break;
        case DeviceChangeAction::None: break;
    }
    if (decideDeviceChange(f) != DeviceChangeAction::None) watchdogTick(app);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App& app = *gApp;
    if (app.taskbarCreatedMsg != 0 && msg == app.taskbarCreatedMsg) {
        addTrayIcon(app);
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            // NOTIFYICON_VERSION_4: событие в LOWORD(lp), координаты в wp. Правый клик приходит
            // и как WM_RBUTTONUP, и как WM_CONTEXTMENU: меню только по второму.
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU: showMenu(app, GET_X_LPARAM(wp), GET_Y_LPARAM(wp)); break;
                case NIN_SELECT:
                case NIN_KEYSELECT: showStatusWindow(app); break;
                default: break;
            }
            return 0;
        case WM_SHOW_STATUS: showStatusWindow(app); return 0;
        case WM_COMMAND: handleCommand(app, LOWORD(wp)); return 0;
        case WM_DEVICE_CHANGE:
            // Одно подключение даёт несколько событий подряд: собираем их kDeviceDebounceMs.
            if (wp == WPARAM(ws::DeviceNotifier::Event::DefaultChanged)) app.defaultChangedPending = true;
            SetTimer(h, kDeviceTimer, kDeviceDebounceMs, nullptr);
            return 0;
        case WM_TIMER:
            if (wp == kWatchdogTimer) watchdogTick(app);
            if (wp == kDeviceTimer) {
                KillTimer(h, kDeviceTimer);
                onDevicesChanged(app);
            }
            return 0;
        case WM_DESTROY:
            KillTimer(h, kWatchdogTimer);
            KillTimer(h, kDeviceTimer);
            app.notifier.stop();
            app.engine.stop();
            Shell_NotifyIconW(NIM_DELETE, &app.nid);
            logLine(app, "exit");
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Дамп падения рядом с логом: bomboec-<время>-<pid>.dmp (разбирается с bomboec.pdb).
    bomboec::crash::install(exeDir());

    // Один экземпляр: второй запуск показывает окно статуса первого. Первый мог
    // захватить mutex, но ещё не создать окно: ждём его до 2 с.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        for (int i = 0; i < 20; ++i) {
            if (HWND other = FindWindowW(kTrayClass, nullptr)) {
                PostMessageW(other, WM_SHOW_STATUS, 0, 0);
                break;
            }
            Sleep(100);
        }
        CloseHandle(mutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        App app;
        gApp = &app;
        app.configPath = exeDir() / "bomboec.toml";
        app.statePath = exeDir() / "bomboec.state.toml";
        app.logPath = exeDir() / "bomboec.log";
        logLine(app, "start " BOMBOEC_VERSION_FULL);

        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = kTrayClass;
        RegisterClassW(&wc);
        WNDCLASSW sc{};
        sc.lpfnWndProc = StatusWndProc;
        sc.hInstance = hInst;
        sc.lpszClassName = L"BomboecStatus";
        sc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&sc);

        // Скрытое top-level окно (см. комментарий в начале файла), никогда не показывается.
        app.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayClass, L"bomboec", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                   hInst, nullptr);
        app.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
        // С повышенными правами UIPI иначе отфильтрует это сообщение от explorer.exe и
        // WM_SHOW_STATUS от второго экземпляра без повышения.
        ChangeWindowMessageFilterEx(app.hwnd, app.taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);
        ChangeWindowMessageFilterEx(app.hwnd, WM_SHOW_STATUS, MSGFLT_ALLOW, nullptr);

        app.nid.cbSize = sizeof(app.nid);
        app.nid.hWnd = app.hwnd;
        app.nid.uID = 1;
        app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        app.nid.uCallbackMessage = WM_TRAY;
        app.nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        app.nid.uVersion = NOTIFYICON_VERSION_4;
        wcscpy_s(app.nid.szTip, L"bomboec");
        addTrayIcon(app);

        std::string error;
        const bool firstRun = !std::filesystem::exists(app.configPath);
        if (!loadOrCreateConfig(app, error)) {
            // Без конфига не стартуем и не повторяем: watchdog запустил бы пустую цепочку.
            app.wantRunning = false;
            app.lastError = error;
            notify(app, L"bomboec: config error", error, NIIF_ERROR);
            showStatusWindow(app);
        } else {
            startEngine(app, true);
        }
        if (firstRun) showStatusWindow(app);
        SetTimer(app.hwnd, kWatchdogTimer, 1000, nullptr);
        {
            // Уведомления об устройствах: колбэк в MTA-потоке только пересылает событие окну.
            const HWND hwnd = app.hwnd;
            std::string nerror;
            if (!app.notifier.start(
                    [hwnd](const ws::DeviceNotifier::Notification& n) {
                        PostMessageW(hwnd, WM_DEVICE_CHANGE, WPARAM(n.event), 0);
                    },
                    nerror)) {
                logLine(app, "device notifications unavailable: " + nerror);
            }
        }

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        gApp = nullptr;
        if (app.statusFont) DeleteObject(app.statusFont);
    }  // App (Engine и COM-объекты внутри) разрушается до CoUninitialize
    CoUninitialize();
    CloseHandle(mutex);
    return 0;
}
