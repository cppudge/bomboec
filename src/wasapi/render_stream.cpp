#include "wasapi/render_stream.h"

#include <avrt.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bomboec::wasapi {

RenderStream::~RenderStream() { close(); }

bool RenderStream::open(IMMDevice* device, const Options& options, FillHandler handler, std::string& error) {
    close();
    options_ = options;
    handler_ = std::move(handler);

    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
    if (FAILED(hr)) {
        error = "IAudioClient::Activate (render): " + hresultToString(hr);
        return false;
    }

    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = WORD(options.channels);
    f.Format.nSamplesPerSec = options.sampleRate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = WORD(options.channels * 4);
    f.Format.nAvgBytesPerSec = options.sampleRate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = options.channels == 1   ? SPEAKER_FRONT_CENTER
                      : options.channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                              : 0;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, REFERENCE_TIME(options.bufferMs) * 10'000, 0, &f.Format,
                             nullptr);
    if (FAILED(hr)) {
        error = "IAudioClient::Initialize (render): " + hresultToString(hr);
        client_.Reset();
        return false;
    }
    event_ = Handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    hr = client_->SetEventHandle(event_.get());
    if (FAILED(hr)) {
        error = "SetEventHandle (render): " + hresultToString(hr);
        return false;
    }
    hr = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(hr)) {
        error = "GetBufferSize (render): " + hresultToString(hr);
        return false;
    }
    REFERENCE_TIME defaultPeriod = 100'000, minPeriod = 0;
    client_->GetDevicePeriod(&defaultPeriod, &minPeriod);
    periodFrames_ = std::max<uint32_t>(1, uint32_t(std::lround(double(defaultPeriod) * options.sampleRate / 1e7)));
    if (options.targetMs == 0) {
        targetFrames_ = bufferFrames_;
    } else {
        targetFrames_ = std::clamp<uint32_t>(uint32_t(uint64_t(options.targetMs) * options.sampleRate / 1000),
                                             periodFrames_, bufferFrames_);
    }
    hr = client_->GetService(IID_PPV_ARGS(&render_));
    if (FAILED(hr)) {
        error = "IAudioRenderClient: " + hresultToString(hr);
        return false;
    }
    stopEvent_ = Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    threadError_.clear();
    hasError_.store(false);
    return true;
}

bool RenderStream::start(std::string& error) {
    if (!client_) {
        error = "render stream not opened";
        return false;
    }
    if (thread_.joinable()) {
        if (!hasError_.load()) return true;  // уже работает
        stop();                              // поток завершился с ошибкой: собрать и попробовать снова
    }
    // Предзаполняем тишиной до целевого уровня, чтобы старт был без щелчка
    // и без лишней задержки. После stop() в буфере может остаться недоигранное.
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (SUCCEEDED(hr) && padding < targetFrames_) {
        BYTE* data = nullptr;
        const UINT32 frames = targetFrames_ - padding;
        hr = render_->GetBuffer(frames, &data);
        if (SUCCEEDED(hr)) hr = render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    if (FAILED(hr)) {
        error = "prefill (render): " + hresultToString(hr);
        return false;
    }
    ResetEvent(stopEvent_.get());
    hr = client_->Start();
    if (FAILED(hr)) {
        error = "IAudioClient::Start (render): " + hresultToString(hr);
        return false;
    }
    threadError_.clear();
    hasError_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { threadMain(); });
    return true;
}

void RenderStream::stop() {
    // join без условий: поток мог завершиться сам после ошибки устройства.
    running_.store(false);
    if (stopEvent_) SetEvent(stopEvent_.get());
    if (thread_.joinable()) thread_.join();
    if (client_) client_->Stop();
}

void RenderStream::close() {
    stop();
    render_.Reset();
    client_.Reset();
    event_.reset();
    stopEvent_.reset();
}

std::string RenderStream::lastError() const { return hasError_.load() ? threadError_ : std::string(); }

void RenderStream::fail(std::string text) {
    threadError_ = std::move(text);
    hasError_.store(true);
}

bool RenderStream::fillOnce() {
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        fail("GetCurrentPadding: " + hresultToString(hr));
        return false;
    }
    // Дозаполняем только до цели: всё, что лежит в буфере сверх периода,
    // это задержка. Полный буфер нужен лишь как ёмкость на случай
    // позднего пробуждения.
    const UINT32 frames = padding < targetFrames_ ? targetFrames_ - padding : 0;
    if (frames == 0) return true;
    BYTE* data = nullptr;
    hr = render_->GetBuffer(frames, &data);
    if (FAILED(hr)) {
        fail("IAudioRenderClient::GetBuffer: " + hresultToString(hr));
        return false;
    }
    if (handler_) {
        handler_(reinterpret_cast<float*>(data), frames);
        hr = render_->ReleaseBuffer(frames, 0);
    } else {
        hr = render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    if (FAILED(hr)) {
        fail("IAudioRenderClient::ReleaseBuffer: " + hresultToString(hr));
        return false;
    }
    return true;
}

void RenderStream::threadMain() {
    const ComInit com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    mmcss_.store(mmcss != nullptr);
    HANDLE waits[2] = {stopEvent_.get(), event_.get()};

    while (running_.load()) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_TIMEOUT) continue;
        if (w != WAIT_OBJECT_0 + 1) {
            // WAIT_FAILED: без выхода цикл крутился бы без ожидания на приоритете MMCSS.
            fail("WaitForMultipleObjects (render): " + hresultToString(HRESULT_FROM_WIN32(GetLastError())));
            break;
        }
        if (!fillOnce()) break;
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace bomboec::wasapi
