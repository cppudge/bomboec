#pragma once

#include "wasapi/com_util.h"

#include <mmdeviceapi.h>

#include <string>
#include <vector>

namespace bomboec::wasapi {

enum class Flow { Capture, Render };

struct DeviceInfo {
    std::wstring id;          // endpoint ID string, стабилен между запусками
    std::wstring name;        // friendly name endpoint'а: "Microphone (bomboec Cable)"
    std::wstring adapter;     // имя устройства, общее для его endpoint'ов: "bomboec Cable"
    std::wstring enumerator;  // шина устройства: USB, HDAUDIO, ROOT (программное), ...
    bool isDefault = false;
};

std::vector<DeviceInfo> enumerateDevices(Flow flow, std::string& error);

// id пустой -> default endpoint (eConsole).
ComPtr<IMMDevice> openDevice(Flow flow, const std::wstring& id, std::string& error);

DeviceInfo describeDevice(IMMDevice* device);

// Два endpoint'а одного программного устройства (virtual cable: Speakers и Microphone
// одного адаптера): микрофон такого устройства при выходе в него же даёт цифровую петлю.
// Физические устройства не считаются: микрофон и наушники гарнитуры связаны только
// акустически. ContainerId для этого не годится: у всех программных устройств он общий.
bool sameVirtualDevice(const DeviceInfo& a, const DeviceInfo& b);

}  // namespace bomboec::wasapi
