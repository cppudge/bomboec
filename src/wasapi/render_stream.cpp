#include "wasapi/render_stream.h"

#include <avrt.h>
#include <ksmedia.h>
#include <mmreg.h>

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
    f.dwChannelMask = options.channels == 1 ? SPEAKER_FRONT_CENTER
                    : options.channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, REFERENCE_TIME(options.bufferMs) * 10'000, 0,
                             &f.Format, nullptr);
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
    client_->GetBufferSize(&bufferFrames_);
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
    if (running_.load()) return true;
    // Предзаполняем буфер тишиной, чтобы старт был без щелчка.
    BYTE* data = nullptr;
    if (SUCCEEDED(render_->GetBuffer(bufferFrames_, &data))) {
        render_->ReleaseBuffer(bufferFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    ResetEvent(stopEvent_.get());
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        error = "IAudioClient::Start (render): " + hresultToString(hr);
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this] { threadMain(); });
    return true;
}

void RenderStream::stop() {
    if (!running_.load()) return;
    running_.store(false);
    SetEvent(stopEvent_.get());
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

std::string RenderStream::lastError() const {
    return hasError_.load() ? threadError_ : std::string();
}

void RenderStream::threadMain() {
    ComInit com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    HANDLE waits[2] = {stopEvent_.get(), event_.get()};

    while (running_.load()) {
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, 2000);
        if (w == WAIT_OBJECT_0) break;
        if (w != WAIT_OBJECT_0 + 1) continue;

        UINT32 padding = 0;
        HRESULT hr = client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            threadError_ = "GetCurrentPadding: " + hresultToString(hr);
            hasError_.store(true);
            break;
        }
        const UINT32 frames = bufferFrames_ - padding;
        if (frames == 0) continue;
        BYTE* data = nullptr;
        hr = render_->GetBuffer(frames, &data);
        if (FAILED(hr)) {
            threadError_ = "IAudioRenderClient::GetBuffer: " + hresultToString(hr);
            hasError_.store(true);
            break;
        }
        if (handler_) {
            handler_(reinterpret_cast<float*>(data), frames);
            render_->ReleaseBuffer(frames, 0);
        } else {
            render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace bomboec::wasapi
