#include "wasapi/devices.h"

#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

namespace bomboec::wasapi {

namespace {

EDataFlow toFlow(Flow f) { return f == Flow::Capture ? eCapture : eRender; }

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
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
            info.name = v.pwszVal;
        }
        PropVariantClear(&v);
    }
    return info;
}

std::vector<DeviceInfo> enumerateDevices(Flow flow, std::string& error) {
    std::vector<DeviceInfo> out;
    ComPtr<IMMDeviceEnumerator> en = makeEnumerator(error);
    if (!en) return out;

    std::wstring defaultId;
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(toFlow(flow), eConsole, &def))) {
        defaultId = deviceId(def.Get());
    }

    ComPtr<IMMDeviceCollection> coll;
    HRESULT hr = en->EnumAudioEndpoints(toFlow(flow), DEVICE_STATE_ACTIVE, &coll);
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
    ComPtr<IMMDeviceEnumerator> en = makeEnumerator(error);
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
