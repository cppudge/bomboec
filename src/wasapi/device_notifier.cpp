#include "wasapi/device_notifier.h"

#include <atomic>
#include <utility>

namespace bomboec::wasapi {

// Реализация IMMNotificationClient с ручным подсчётом ссылок: объект живёт, пока на
// него ссылается MMDevice API (после UnregisterEndpointNotificationCallback он
// отпускается) и DeviceNotifier.
class DeviceNotifier::Client final : public IMMNotificationClient {
public:
    explicit Client(Handler handler) : handler_(std::move(handler)) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD newState) override {
        emit({Event::StateChanged, Flow::Render, id ? id : L"", newState});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override {
        emit({Event::Added, Flow::Render, id ? id : L"", 0});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        emit({Event::Removed, Flow::Render, id ? id : L"", 0});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        // Движок открывает устройства по умолчанию в роли eConsole; eMultimedia и
        // eCommunications приходят теми же событиями, достаточно одного.
        if (role != eConsole) return S_OK;
        emit({Event::DefaultChanged, flow == eCapture ? Flow::Capture : Flow::Render, id ? id : L"", 0});
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

    void detach() { active_.store(false); }

private:
    ~Client() = default;

    void emit(const Notification& n) {
        if (active_.load()) handler_(n);
    }

    Handler handler_;
    std::atomic<ULONG> refs_{1};
    std::atomic<bool> active_{true};
};

DeviceNotifier::~DeviceNotifier() { stop(); }

bool DeviceNotifier::start(Handler handler, std::string& error) {
    stop();
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr)) {
        error = "MMDeviceEnumerator: " + hresultToString(hr);
        return false;
    }
    client_ = new Client(std::move(handler));
    hr = enumerator_->RegisterEndpointNotificationCallback(client_);
    if (FAILED(hr)) {
        error = "RegisterEndpointNotificationCallback: " + hresultToString(hr);
        client_->Release();
        client_ = nullptr;
        enumerator_.Reset();
        return false;
    }
    return true;
}

void DeviceNotifier::stop() {
    if (client_) {
        // Колбэк мог выполняться прямо сейчас в чужом потоке: после detach он ничего не
        // делает, а Unregister возвращается, когда MMDevice API отпустил объект.
        client_->detach();
        if (enumerator_) enumerator_->UnregisterEndpointNotificationCallback(client_);
        client_->Release();
        client_ = nullptr;
    }
    enumerator_.Reset();
}

}  // namespace bomboec::wasapi
