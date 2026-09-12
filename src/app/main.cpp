// bomboec: tray-приложение вокруг Engine. Политика (конфиг, старт и остановка, watchdog,
// устройства) живёт в app/Controller и проверяется тестами; здесь только Win32: скрытое
// окно, иконка и меню трея, окно статуса, лог, уведомления, таймеры, уведомления системы
// об устройствах.
//
// Каталог данных (app/paths.h): рядом с exe, если там есть bomboec.toml или маркер `portable`,
// иначе %LOCALAPPDATA%\bomboec. В нём bomboec.toml (создаётся из config/default.toml,
// встроенного при сборке), bomboec.state.toml, bomboec.log и минидампы. Один экземпляр:
// повторный запуск показывает окно статуса уже работающего.
//
// Окно приложения скрытое top-level, а не message-only: только такое получает
// TaskbarCreated (иконка возвращается после перезапуска explorer.exe) и находится
// через FindWindow из второго экземпляра.

#include "app/controller.h"
#include "app/crash_dump.h"
#include "app/paths.h"
#include "core/utf8.h"
#include "engine/engine.h"
#include "version.h"
#include "wasapi/com_util.h"
#include "wasapi/device_notifier.h"
#include "wasapi/devices.h"

#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace bomboec;
namespace ws = bomboec::wasapi;
using bomboec::app::Controller;
using bomboec::app::IHost;

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

// Engine за интерфейсом контроллера.
class RealEngine final : public bomboec::app::IEngine {
public:
    bool start(const AppConfig& cfg, std::string& error) override { return engine_.start(cfg, error); }
    void stop() override { engine_.stop(); }
    bool running() const override { return engine_.running(); }
    EngineStatus status() const override { return engine_.status(); }
    bool reopenReference(std::string& error) override { return engine_.reopenReference(error); }

private:
    Engine engine_;
};

struct App;
void showStatusWindow(App& app);
void updateTooltip(App& app);
void logLine(App& app, const std::string& text);

// Лог, уведомления, устройства и часы для контроллера.
class Win32Host final : public IHost {
public:
    explicit Win32Host(App& app) : app_(app) {}
    void log(const std::string& line) override { logLine(app_, line); }
    void notify(const std::string& title, const std::string& text, Level level) override;
    void showStatus() override { showStatusWindow(app_); }
    std::vector<ws::DeviceInfo> devices(ws::Flow flow) override {
        std::string error;
        return ws::enumerateDevices(flow, error);
    }
    uint64_t nowMs() override { return GetTickCount64(); }
    void engineStateChanged() override { updateTooltip(app_); }

private:
    App& app_;
};

struct App {
    HWND hwnd = nullptr;
    HWND statusWnd = nullptr;
    HWND statusEdit = nullptr;
    NOTIFYICONDATAW nid{};
    bool trayAdded = false;
    UINT taskbarCreatedMsg = 0;  // RegisterWindowMessage(L"TaskbarCreated")
    HFONT statusFont = nullptr;
    std::filesystem::path configPath, logPath;
    RealEngine engine;
    Win32Host host{*this};
    std::unique_ptr<Controller> controller;
    ws::DeviceNotifier notifier;
    bool defaultChangedPending = false;  // среди собранных уведомлений была смена default
    // Списки устройств для открытого меню: пункты меню индексируют именно их. Контроллер
    // за время, пока меню открыто, может перечислить устройства заново, поэтому у него свои.
    std::vector<ws::DeviceInfo> menuCapDevices, menuRenDevices;
};

App* gApp = nullptr;

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

void Win32Host::notify(const std::string& title, const std::string& text, Level level) {
    logLine(app_, title + ": " + text);
    NOTIFYICONDATAW n = app_.nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = level == Level::Error ? NIIF_ERROR : level == Level::Warning ? NIIF_WARNING : NIIF_INFO;
    wcsncpy_s(n.szInfoTitle, ws::fromUtf8(title).c_str(), _TRUNCATE);
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

std::string statusText(App& app) {
    const EngineStatus s = app.controller->status();
    char buf[2048];
    if (!s.running) {
        std::snprintf(buf, sizeof(buf),
                      "bomboec %s: stopped\r\n\r\n%s\r\n\r\n"
                      "Правый клик по иконке в трее: выбор микрофона, колонок и выхода, старт.\r\n"
                      "Конфиг: %s\r\nЛог: %s",
                      BOMBOEC_VERSION_FULL, app.controller->lastError().c_str(), pathToUtf8(app.configPath).c_str(),
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
    const EngineSettings& dev = app.controller->config().engine;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (app.engine.running() ? MF_CHECKED : 0), ID_TOGGLE,
                app.engine.running() ? L"Running (click to stop)" : L"Start");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    appendDeviceMenu(menu, L"Microphone", app.menuCapDevices, dev.micId, ID_MIC_BASE);
    appendDeviceMenu(menu, L"Speakers (reference)", app.menuRenDevices, dev.speakersId, ID_SPK_BASE);
    appendDeviceMenu(menu, L"Output (virtual mic)", app.menuRenDevices, dev.outputId, ID_OUT_BASE);
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

// Пункт подменю устройств: 0 = по умолчанию, дальше индекс в списке меню.
std::optional<ws::DeviceInfo> menuDevice(const std::vector<ws::DeviceInfo>& devices, UINT index) {
    if (index == 0 || index - 1 >= devices.size()) return std::nullopt;
    return devices[index - 1];
}

void handleCommand(App& app, UINT id) {
    Controller& ctl = *app.controller;
    if (id == ID_TOGGLE) {
        ctl.toggle();
    } else if (id == ID_STATUS) {
        showStatusWindow(app);
    } else if (id == ID_CONFIG) {
        ShellExecuteW(nullptr, L"open", app.configPath.c_str(), nullptr, nullptr, SW_SHOW);
    } else if (id == ID_LOG) {
        ShellExecuteW(nullptr, L"open", app.logPath.c_str(), nullptr, nullptr, SW_SHOW);
    } else if (id == ID_RELOAD) {
        ctl.reload();
    } else if (id == ID_EXIT) {
        DestroyWindow(app.hwnd);
    } else if (id >= ID_MIC_BASE && id < ID_MIC_BASE + 1000) {
        ctl.selectDevice(Controller::Slot::Mic, menuDevice(app.menuCapDevices, id - ID_MIC_BASE));
    } else if (id >= ID_SPK_BASE && id < ID_SPK_BASE + 1000) {
        ctl.selectDevice(Controller::Slot::Speakers, menuDevice(app.menuRenDevices, id - ID_SPK_BASE));
    } else if (id >= ID_OUT_BASE && id < ID_OUT_BASE + 1000) {
        ctl.selectDevice(Controller::Slot::Output, menuDevice(app.menuRenDevices, id - ID_OUT_BASE));
    }
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
            if (wp == kWatchdogTimer) {
                if (!app.trayAdded) addTrayIcon(app);
                app.controller->tick();
            }
            if (wp == kDeviceTimer) {
                KillTimer(h, kDeviceTimer);
                const bool defaultChanged = app.defaultChangedPending;
                app.defaultChangedPending = false;
                app.controller->onDevicesChanged(defaultChanged);
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
    const std::filesystem::path dataDir = bomboec::app::dataDirectory();
    bomboec::crash::install(dataDir);

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
        app.configPath = dataDir / "bomboec.toml";
        app.logPath = dataDir / "bomboec.log";
        app.controller =
            std::make_unique<Controller>(app.engine, app.host, app.configPath, dataDir / "bomboec.state.toml");
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
        if (!app.controller->loadOrCreateConfig(error)) {
            // Без конфига не стартуем и не повторяем: watchdog запустил бы пустую цепочку.
            app.controller->disable(error);
            app.host.notify("bomboec: config error", error, IHost::Level::Error);
            showStatusWindow(app);
        } else {
            app.controller->start(true);
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
