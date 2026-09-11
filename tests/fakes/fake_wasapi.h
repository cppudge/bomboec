#pragma once

// Поддельные WASAPI-объекты для тестов CaptureStream и RenderStream: пути
// ошибок устройства (выдернули USB, сменили формат) без реального звука.

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <atomic>
#include <cstring>
#include <vector>

namespace bomboec::test {

// Поведение поддельного устройства. Должно жить дольше потоков, которые его используют.
struct FakeAudio {
    UINT32 mixChannels = 2;
    UINT32 acceptChannels = 0;  // 0: Initialize принимает любое число каналов
    UINT32 bufferFrames = 9600;
    UINT32 packetFrames = 480;
    // Что возвращают GetNextPacketSize / GetBuffer (захват) и GetCurrentPadding (render).
    std::atomic<HRESULT> streamError{S_OK};
    // Захват: сколько пакетов готово; значение канала c в пакете равно c + 1.
    std::atomic<int> pendingPackets{0};
    std::atomic<int> starts{0};
    std::atomic<UINT32> renderedFrames{0};
    std::atomic<HANDLE> event{nullptr};  // событие последнего клиента

    void signal() {
        if (HANDLE h = event.load()) SetEvent(h);
    }
};

template <typename Iface> class FakeUnknown : public Iface {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(Iface)) {
            *ppv = static_cast<Iface*>(this);
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

protected:
    virtual ~FakeUnknown() = default;

private:
    std::atomic<ULONG> refs_{1};
};

class FakeCapture final : public FakeUnknown<IAudioCaptureClient> {
public:
    FakeCapture(FakeAudio& audio, UINT32 channels)
        : audio_(audio), channels_(channels), buf_(size_t(audio.packetFrames) * channels) {}

    HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, UINT64* devicePosition,
                                        UINT64* qpcPosition) override {
        const HRESULT err = audio_.streamError.load();
        if (FAILED(err)) return err;
        if (audio_.pendingPackets.load() <= 0) {
            *frames = 0;
            return AUDCLNT_S_BUFFER_EMPTY;
        }
        for (UINT32 i = 0; i < audio_.packetFrames; ++i) {
            for (UINT32 c = 0; c < channels_; ++c) buf_[size_t(i) * channels_ + c] = float(c + 1);
        }
        *data = reinterpret_cast<BYTE*>(buf_.data());
        *frames = audio_.packetFrames;
        *flags = 0;
        if (devicePosition) *devicePosition = position_;
        if (qpcPosition) *qpcPosition = 10'000'000 + position_ * 10'000'000 / 48'000;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames) override {
        if (frames > 0) {
            --audio_.pendingPackets;
            position_ += frames;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNextPacketSize(UINT32* frames) override {
        const HRESULT err = audio_.streamError.load();
        if (FAILED(err)) return err;
        *frames = audio_.pendingPackets.load() > 0 ? audio_.packetFrames : 0;
        return S_OK;
    }

private:
    FakeAudio& audio_;
    UINT32 channels_;
    std::vector<float> buf_;
    UINT64 position_ = 0;
};

class FakeRender final : public FakeUnknown<IAudioRenderClient> {
public:
    FakeRender(FakeAudio& audio, UINT32 channels) : audio_(audio), buf_(size_t(audio.bufferFrames) * channels) {}

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames, BYTE** data) override {
        if (frames > audio_.bufferFrames) return AUDCLNT_E_BUFFER_TOO_LARGE;
        *data = reinterpret_cast<BYTE*>(buf_.data());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames, DWORD) override {
        audio_.renderedFrames += frames;
        return S_OK;
    }

private:
    FakeAudio& audio_;
    std::vector<float> buf_;
};

class FakeClient final : public FakeUnknown<IAudioClient> {
public:
    explicit FakeClient(FakeAudio& audio) : audio_(audio) {}

    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME,
                                         const WAVEFORMATEX* format, LPCGUID) override {
        if (audio_.acceptChannels != 0 && format->nChannels != audio_.acceptChannels) {
            return AUDCLNT_E_UNSUPPORTED_FORMAT;
        }
        channels_ = format->nChannels;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override {
        *frames = audio_.bufferFrames;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME* latency) override {
        *latency = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* padding) override {
        const HRESULT err = audio_.streamError.load();
        if (FAILED(err)) return err;
        *padding = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** format) override {
        auto* f = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        std::memset(f, 0, sizeof(*f));
        f->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        f->nChannels = WORD(audio_.mixChannels);
        f->nSamplesPerSec = 48000;
        f->wBitsPerSample = 32;
        f->nBlockAlign = WORD(4 * audio_.mixChannels);
        f->nAvgBytesPerSec = 48000 * f->nBlockAlign;
        *format = f;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* defaultPeriod, REFERENCE_TIME* minPeriod) override {
        if (defaultPeriod) *defaultPeriod = 100'000;
        if (minPeriod) *minPeriod = 30'000;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Start() override {
        ++audio_.starts;
        audio_.signal();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event) override {
        audio_.event.store(event);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetService(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IAudioCaptureClient)) {
            *ppv = static_cast<IAudioCaptureClient*>(new FakeCapture(audio_, channels_));
            return S_OK;
        }
        if (riid == __uuidof(IAudioRenderClient)) {
            *ppv = static_cast<IAudioRenderClient*>(new FakeRender(audio_, channels_));
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

private:
    FakeAudio& audio_;
    UINT32 channels_ = 0;
};

class FakeDevice final : public FakeUnknown<IMMDevice> {
public:
    explicit FakeDevice(FakeAudio& audio) : audio_(audio) {}

    HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD, PROPVARIANT*, void** ppv) override {
        if (iid == __uuidof(IAudioClient)) {
            *ppv = static_cast<IAudioClient*>(new FakeClient(audio_));
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetState(DWORD* state) override {
        *state = DEVICE_STATE_ACTIVE;
        return S_OK;
    }

private:
    FakeAudio& audio_;
};

inline Microsoft::WRL::ComPtr<IMMDevice> makeFakeDevice(FakeAudio& audio) {
    Microsoft::WRL::ComPtr<IMMDevice> dev;
    dev.Attach(new FakeDevice(audio));
    return dev;
}

}  // namespace bomboec::test
