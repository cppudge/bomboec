#pragma once

#include <windows.h>
#include <objbase.h>
#include <wrl/client.h>

#include <string>

namespace bomboec::wasapi {

using Microsoft::WRL::ComPtr;

// COM для текущего потока (MTA). Каждый поток, работающий с WASAPI, держит свой.
class ComInit {
public:
    ComInit() { hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInit() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;

private:
    HRESULT hr_ = E_FAIL;
};

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }
    void reset() {
        if (h_) CloseHandle(h_);
        h_ = nullptr;
    }
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HANDLE h_ = nullptr;
};

std::string hresultToString(HRESULT hr);
std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);

// QPC в единицах 100 ns, как в метках пакетов WASAPI.
int64_t qpcNow100ns();

}  // namespace bomboec::wasapi
