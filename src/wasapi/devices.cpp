#include "wasapi/devices.h"

#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include <cwchar>

namespace bomboec::wasapi {

namespace {

// PKEY_Device_EnumeratorName в devpkey.h объявлен как DEVPROPKEY, IPropertyStore нужен PROPERTYKEY.
constexpr PROPERTYKEY kEnumeratorName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
                                         24};

EDataFlow toFlow(Flow f) { return f == Flow::Capture ? eCapture : eRender; }

std::wstring readString(IPropertyStore* props, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_LPWSTR) out = v.pwszVal;
    PropVariantClear(&v);
    return out;
}

ComPtr<IMMDeviceEnumerator> makeEnumerator(std::string& error) {
    ComPtr<IMMDeviceEnumerator> en;
    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) error = "MMDeviceEnumerator: " + hresultToString(hr);
    return en;
}

std::wstring deviceId(IMMDevice* device) {
    LPWSTR id = nullptr;
    std::wstring out;
    if (SUCCEEDED(device->GetId(&id)) && id) {
        out = id;
        CoTaskMemFree(id);
    }
    return out;
}

}  // namespace

DeviceInfo describeDevice(IMMDevice* device) {
    DeviceInfo info;
    info.id = deviceId(device);
    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
        info.name = readString(props.Get(), PKEY_Device_FriendlyName);
        info.adapter = readString(props.Get(), PKEY_DeviceInterface_FriendlyName);
        info.enumerator = readString(props.Get(), kEnumeratorName);
    }
    return info;
}

bool sameVirtualDevice(const DeviceInfo& a, const DeviceInfo& b) {
    const auto isVirtual = [](const std::wstring& e) {
        return _wcsicmp(e.c_str(), L"ROOT") == 0 || _wcsicmp(e.c_str(), L"SWD") == 0;
    };
    return !a.adapter.empty() && a.adapter == b.adapter && isVirtual(a.enumerator) && isVirtual(b.enumerator);
}

std::vector<DeviceInfo> enumerateDevices(Flow flow, std::string& error) {
    std::vector<DeviceInfo> out;
    const ComPtr<IMMDeviceEnumerator> en = makeEnumerator(error);
    if (!en) return out;

    std::wstring defaultId;
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(toFlow(flow), eConsole, &def))) {
        defaultId = deviceId(def.Get());
    }

    ComPtr<IMMDeviceCollection> coll;
    const HRESULT hr = en->EnumAudioEndpoints(toFlow(flow), DEVICE_STATE_ACTIVE, &coll);
    if (FAILED(hr)) {
        error = "EnumAudioEndpoints: " + hresultToString(hr);
        return out;
    }
    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;
        DeviceInfo info = describeDevice(dev.Get());
        info.isDefault = (info.id == defaultId);
        out.push_back(std::move(info));
    }
    return out;
}

ComPtr<IMMDevice> openDevice(Flow flow, const std::wstring& id, std::string& error) {
    ComPtr<IMMDevice> dev;
    const ComPtr<IMMDeviceEnumerator> en = makeEnumerator(error);
    if (!en) return dev;
    HRESULT hr;
    if (id.empty()) {
        hr = en->GetDefaultAudioEndpoint(toFlow(flow), eConsole, &dev);
    } else {
        hr = en->GetDevice(id.c_str(), &dev);
    }
    if (FAILED(hr)) {
        error = "open device '" + toUtf8(id) + "': " + hresultToString(hr);
        dev.Reset();
    }
    return dev;
}

}  // namespace bomboec::wasapi
