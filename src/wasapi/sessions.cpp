#include "wasapi/sessions.h"

#include <audiopolicy.h>

#include <algorithm>

namespace bomboec::wasapi {

namespace {

std::string processName(DWORD pid) {
    std::string fallback = "pid " + std::to_string(pid);
    const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return fallback;
    wchar_t path[MAX_PATH];
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(h, 0, path, &size) != 0;
    CloseHandle(h);
    if (!ok) return fallback;
    const std::wstring full(path, size);
    const size_t slash = full.find_last_of(L'\\');
    return toUtf8(slash == std::wstring::npos ? full : full.substr(slash + 1));
}

}  // namespace

bool activeSessionProcesses(IMMDevice* device, uint32_t excludePid, std::vector<std::string>& names,
                            std::string& error) {
    names.clear();
    ComPtr<IAudioSessionManager2> manager;
    HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager);
    if (FAILED(hr)) {
        error = "IAudioSessionManager2: " + hresultToString(hr);
        return false;
    }
    ComPtr<IAudioSessionEnumerator> sessions;
    hr = manager->GetSessionEnumerator(&sessions);
    if (FAILED(hr)) {
        error = "GetSessionEnumerator: " + hresultToString(hr);
        return false;
    }
    int count = 0;
    hr = sessions->GetCount(&count);
    if (FAILED(hr)) {
        error = "GetCount: " + hresultToString(hr);
        return false;
    }
    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessions->GetSession(i, &control))) continue;
        AudioSessionState state = AudioSessionStateInactive;
        if (FAILED(control->GetState(&state)) || state != AudioSessionStateActive) continue;
        ComPtr<IAudioSessionControl2> control2;
        if (FAILED(control.As(&control2))) continue;
        DWORD pid = 0;
        if (FAILED(control2->GetProcessId(&pid)) || pid == excludePid) continue;
        if (control2->IsSystemSoundsSession() == S_OK) continue;
        const std::string name = processName(pid);
        if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    return true;
}

}  // namespace bomboec::wasapi
