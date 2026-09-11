#pragma once

#include "wasapi/com_util.h"

#include <mmdeviceapi.h>

#include <string>
#include <vector>

namespace bomboec::wasapi {

enum class Flow { Capture, Render };

struct DeviceInfo {
    std::wstring id;    // endpoint ID string, стабилен между запусками
    std::wstring name;  // friendly name
    bool isDefault = false;
};

std::vector<DeviceInfo> enumerateDevices(Flow flow, std::string& error);

// id пустой -> default endpoint (eConsole).
ComPtr<IMMDevice> openDevice(Flow flow, const std::wstring& id, std::string& error);

DeviceInfo describeDevice(IMMDevice* device);

}  // namespace bomboec::wasapi
