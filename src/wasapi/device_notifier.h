#pragma once

#include "wasapi/com_util.h"
#include "wasapi/devices.h"

#include <mmdeviceapi.h>

#include <functional>
#include <string>

namespace bomboec::wasapi {

// Уведомления системы об аудиоустройствах (IMMNotificationClient): endpoint появился,
// пропал, включён/отключён, сменилось устройство по умолчанию. Колбэк приходит в
// потоке пула MMDevice API (MTA), поэтому он должен только передать событие дальше
// (PostMessage в UI-поток), не трогая движок и не блокируясь.
class DeviceNotifier {
public:
    enum class Event { DefaultChanged, StateChanged, Added, Removed };
    struct Notification {
        Event event;
        Flow flow;        // для DefaultChanged
        std::wstring id;  // endpoint id
        DWORD newState;   // DEVICE_STATE_* для StateChanged
    };
    using Handler = std::function<void(const Notification&)>;

    DeviceNotifier() = default;
    ~DeviceNotifier();
    DeviceNotifier(const DeviceNotifier&) = delete;
    DeviceNotifier& operator=(const DeviceNotifier&) = delete;

    // Из потока с инициализированным COM. handler живёт до stop().
    bool start(Handler handler, std::string& error);
    void stop();

private:
    class Client;
    ComPtr<IMMDeviceEnumerator> enumerator_;
    Client* client_ = nullptr;
};

}  // namespace bomboec::wasapi
