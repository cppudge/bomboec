// bomboec: tray-приложение вокруг Engine.
//
// Конфиг: bomboec.toml рядом с exe (создаётся из встроенного шаблона).
// Лог: bomboec.log рядом с exe. Один экземпляр: повторный запуск показывает
// окно статуса уже работающего.
// Меню в трее: старт/стоп, выбор микрофона, колонок (reference) и выхода,
// окно статуса, открыть конфиг, выход. Выбор устройства сохраняется в конфиг
// (id и имя) и перезапускает движок. Если id устройства больше нет
// (переустановка драйвера кабеля), устройство ищется по имени.

#include "engine/engine.h"
#include "wasapi/com_util.h"
#include "wasapi/devices.h"

#include <windows.h>
#include <shellapi.h>

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
constexpr UINT_PTR kStatusTimer = 1;
constexpr UINT_PTR kWatchdogTimer = 2;
constexpr UINT ID_TOGGLE = 100, ID_STATUS = 101, ID_CONFIG = 102, ID_RELOAD = 103, ID_EXIT = 104, ID_LOG = 105;
constexpr UINT ID_MIC_BASE = 1000, ID_SPK_BASE = 2000, ID_OUT_BASE = 3000;
const wchar_t* kTrayClass = L"BomboecTray";
const wchar_t* kMutexName = L"Local\\bomboec.tray.single";

const char* kDefaultConfig = R"(# bomboec configuration

[format]
sample_rate = 48000
frame_ms = 10
mic_channels = 1
reference_channels = 2

[devices]
mic = ""        # endpoint id; пусто = default capture
speakers = ""   # render endpoint для loopback; пусто = default render
output = ""     # render endpoint для очищенного сигнала (virtual cable)
mic_name = ""
speakers_name = ""
output_name = ""

[engine]
mic_raw = true
reference_lead_ms = 20
output_buffer_ms = 10   # запас в выходном кольце (джиттер микрофона)
output_render_ms = 20   # заполнение буфера WASAPI выхода, не меньше 2 периодов
output_channels = 2
record_dir = ""

[[chain]]
id = "webrtc"
aec = true
hpf = true
ns = false
ns_level = "moderate"
agc = false
filter_length_blocks = 13
delay_num_filters = 5

[[chain]]
id = "limiter"
ceiling_db = -1.0
release_ms = 50.0
)";

struct App {
    HWND hwnd = nullptr;
    HWND statusWnd = nullptr;
    HWND statusEdit = nullptr;
    NOTIFYICONDATAW nid{};
    std::filesystem::path configPath, logPath;
    AppConfig cfg;
    Engine engine;
    std::vector<ws::DeviceInfo> capDevices, renDevices;
    std::string lastError;
    bool wantRunning = true;
};

App* g_app = nullptr;

std::filesystem::path exeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

void logLine(App& app, const std::string& text) {
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

bool loadOrCreateConfig(App& app, std::string& error) {
    if (!std::filesystem::exists(app.configPath)) {
        std::ofstream out(app.configPath, std::ios::binary);
        out << kDefaultConfig;
        logLine(app, "config created: " + app.configPath.string());
    }
    return loadConfig(app.configPath, app.cfg, error);
}

// Если сохранённого id нет среди устройств, ищем по имени и обновляем id.
void resolveDevicesByName(App& app) {
    std::string error;
    app.capDevices = ws::enumerateDevices(ws::Flow::Capture, error);
    app.renDevices = ws::enumerateDevices(ws::Flow::Render, error);
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
    fix(app.cfg.engine.micId, app.cfg.engine.micName, app.capDevices, "mic");
    fix(app.cfg.engine.speakersId, app.cfg.engine.speakersName, app.renDevices, "speakers");
    fix(app.cfg.engine.outputId, app.cfg.engine.outputName, app.renDevices, "output");
    // Выход не выбран: если установлен наш кабель, берём его.
    if (app.cfg.engine.outputId.empty()) {
        for (const ws::DeviceInfo& d : app.renDevices) {
            if (d.name.find(L"bomboec Cable") != std::wstring::npos) {
                app.cfg.engine.outputId = ws::toUtf8(d.id);
                app.cfg.engine.outputName = ws::toUtf8(d.name);
                logLine(app, "output defaulted to '" + app.cfg.engine.outputName + "'");
                changed = true;
                break;
            }
        }
    }
    if (changed) saveConfig(app.configPath, app.cfg, error);
}

void showStatusWindow(App& app);

void startEngine(App& app) {
    resolveDevicesByName(app);
    std::string error;
    if (!app.engine.start(app.cfg, error)) {
        app.lastError = error;
        notify(app, L"bomboec: start failed", error, NIIF_ERROR);
        showStatusWindow(app);
    } else {
        app.lastError.clear();
        const EngineStatus s = app.engine.status();
        logLine(app, "engine started: mic '" + s.micName + "' (raw " + (s.micRaw ? "on" : "off") + "), speakers '" +
                         s.speakersName + "', output '" + s.outputName + "'");
    }
    updateTooltip(app);
}

void stopEngine(App& app) {
    if (app.engine.running()) logLine(app, "engine stopped");
    app.engine.stop();
    updateTooltip(app);
}

void restartIfRunning(App& app) {
    if (app.engine.running()) stopEngine(app);
    if (app.wantRunning) startEngine(app);
}

std::string statusText(App& app) {
    const EngineStatus s = app.engine.status();
    char buf[2048];
    if (!s.running) {
        std::snprintf(
            buf, sizeof(buf),
            "stopped\r\n\r\n%s\r\n\r\nПравый клик по иконке в трее: выбор микрофона, колонок и выхода, старт.\r\n"
            "Конфиг: %s\r\nЛог: %s",
            app.lastError.c_str(), app.configPath.string().c_str(), app.logPath.string().c_str());
        return buf;
    }
    auto opt = [](const std::optional<double>& v, const char* unit) {
        char b[32];
        if (!v) return std::string("n/a");
        std::snprintf(b, sizeof(b), "%.1f %s", *v, unit);
        return std::string(b);
    };
    std::snprintf(buf, sizeof(buf),
                  "running\r\nmic:       %s (raw %s)\r\nreference: %s\r\noutput:    %s\r\n\r\n"
                  "levels     mic %6.1f   ref %6.1f   out %6.1f dBFS\r\n"
                  "aec        delay %s   erl %s   erle %s\r\n"
                  "reference  lead %.0f ms   missing %llu   gaps mic %llu / ref %llu\r\n"
                  "output     ring %u ms (margin %u ms)   wasapi %u ms   underruns %llu   overruns %llu\r\n"
                  "fill ctl   inserted %llu   dropped %llu samples\r\n"
                  "drift      mic %+.0f ppm   ref %+.0f ppm\r\n"
                  "frames     %llu\r\n%s",
                  s.micName.c_str(), s.micRaw ? "on" : "off", s.speakersName.c_str(), s.outputName.c_str(), s.micDb,
                  s.refDb, s.outDb, opt(s.stats.delayMs, "ms").c_str(), opt(s.stats.erlDb, "dB").c_str(),
                  opt(s.stats.erleDb, "dB").c_str(), s.referenceLeadMs, (unsigned long long)s.refMissing,
                  (unsigned long long)s.micGaps, (unsigned long long)s.refGaps, s.outBufferedMs, s.outMarginMs,
                  s.outRenderMs, (unsigned long long)s.outUnderruns, (unsigned long long)s.outOverruns,
                  (unsigned long long)s.outInserted, (unsigned long long)s.outDropped, s.micDriftPpm, s.refDriftPpm,
                  (unsigned long long)s.framesProcessed, s.error.empty() ? "" : ("ERROR: " + s.error).c_str());
    return buf;
}

LRESULT CALLBACK StatusWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App& app = *g_app;
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
    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    SendMessageW(app.statusEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
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

void showMenu(App& app) {
    std::string error;
    app.capDevices = ws::enumerateDevices(ws::Flow::Capture, error);
    app.renDevices = ws::enumerateDevices(ws::Flow::Render, error);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (app.engine.running() ? MF_CHECKED : 0), ID_TOGGLE,
                app.engine.running() ? L"Running (click to stop)" : L"Start");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    appendDeviceMenu(menu, L"Microphone", app.capDevices, app.cfg.engine.micId, ID_MIC_BASE);
    appendDeviceMenu(menu, L"Speakers (reference)", app.renDevices, app.cfg.engine.speakersId, ID_SPK_BASE);
    appendDeviceMenu(menu, L"Output (virtual mic)", app.renDevices, app.cfg.engine.outputId, ID_OUT_BASE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_STATUS, L"Status...");
    AppendMenuW(menu, MF_STRING, ID_CONFIG, L"Open config");
    AppendMenuW(menu, MF_STRING, ID_LOG, L"Open log");
    AppendMenuW(menu, MF_STRING, ID_RELOAD, L"Reload config && restart");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(app.hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, app.hwnd, nullptr);
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
    if (!saveConfig(app.configPath, app.cfg, error)) notify(app, L"bomboec", error, NIIF_WARNING);
    logLine(app, "device selected: '" + name + "' (" + id + ")");
    restartIfRunning(app);
}

void handleCommand(App& app, UINT id) {
    if (id == ID_TOGGLE) {
        app.wantRunning = !app.engine.running();
        if (app.wantRunning) startEngine(app);
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
        selectDevice(app, app.cfg.engine.micId, app.cfg.engine.micName, app.capDevices, id - ID_MIC_BASE);
    } else if (id >= ID_SPK_BASE && id < ID_SPK_BASE + 1000) {
        selectDevice(app, app.cfg.engine.speakersId, app.cfg.engine.speakersName, app.renDevices, id - ID_SPK_BASE);
    } else if (id >= ID_OUT_BASE && id < ID_OUT_BASE + 1000) {
        selectDevice(app, app.cfg.engine.outputId, app.cfg.engine.outputName, app.renDevices, id - ID_OUT_BASE);
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App& app = *g_app;
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu(app);
            else if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) showStatusWindow(app);
            return 0;
        case WM_SHOW_STATUS: showStatusWindow(app); return 0;
        case WM_COMMAND: handleCommand(app, LOWORD(wp)); return 0;
        case WM_TIMER:
            if (wp == kWatchdogTimer && app.wantRunning && app.engine.running()) {
                const EngineStatus s = app.engine.status();
                if (!s.error.empty()) {
                    app.lastError = s.error;
                    notify(app, L"bomboec: restarting", s.error, NIIF_WARNING);
                    stopEngine(app);
                    Sleep(500);
                    startEngine(app);
                }
            }
            return 0;
        case WM_DESTROY:
            KillTimer(h, kWatchdogTimer);
            app.engine.stop();
            Shell_NotifyIconW(NIM_DELETE, &app.nid);
            logLine(app, "exit");
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // Один экземпляр: второй запуск показывает окно статуса первого.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kTrayClass, nullptr)) PostMessageW(other, WM_SHOW_STATUS, 0, 0);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    App app;
    g_app = &app;
    app.configPath = exeDir() / "bomboec.toml";
    app.logPath = exeDir() / "bomboec.log";
    logLine(app, "start");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kTrayClass;
    RegisterClassW(&wc);
    WNDCLASSW sc{};
    sc.lpfnWndProc = StatusWndProc;
    sc.hInstance = hInst;
    sc.lpszClassName = L"BomboecStatus";
    sc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&sc);

    app.hwnd = CreateWindowExW(0, kTrayClass, L"bomboec", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);

    app.nid.cbSize = sizeof(app.nid);
    app.nid.hWnd = app.hwnd;
    app.nid.uID = 1;
    app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    app.nid.uCallbackMessage = WM_TRAY;
    app.nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(app.nid.szTip, L"bomboec");
    Shell_NotifyIconW(NIM_ADD, &app.nid);
    app.nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &app.nid);

    std::string error;
    const bool firstRun = !std::filesystem::exists(app.configPath);
    if (!loadOrCreateConfig(app, error)) {
        app.lastError = error;
        notify(app, L"bomboec: config error", error, NIIF_ERROR);
        showStatusWindow(app);
    } else {
        startEngine(app);
    }
    if (firstRun) showStatusWindow(app);
    SetTimer(app.hwnd, kWatchdogTimer, 1000, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_app = nullptr;
    CoUninitialize();
    CloseHandle(mutex);
    return 0;
}
