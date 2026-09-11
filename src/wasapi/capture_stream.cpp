#include "wasapi/capture_stream.h"

#include <avrt.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <algorithm>
#include <cstring>

namespace bomboec::wasapi {

namespace {

WAVEFORMATEXTENSIBLE makeFloatFormat(uint32_t rate, uint32_t channels) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = WORD(channels);
    f.Format.nSamplesPerSec = rate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = WORD(channels * 4);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER
                    : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                    : 0;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

constexpr REFERENCE_TIME kRefTimesPerMs = 10'000;

}  // namespace

CaptureStream::~CaptureStream() { close(); }

bool CaptureStream::open(IMMDevice* device, const Options& options, PacketHandler handler, std::string& error) {
    close();
    options_ = options;
    handler_ = std::move(handler);

    ComPtr<IAudioClient> client;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) {
        error = "IAudioClient::Activate: " + hresultToString(hr);
        return false;
    }

    // Raw mode: обходим APO микрофона (системные NS/AEC/AGC), чтобы AEC получал
    // необработанный near-end. Для loopback не применяется.
    rawApplied_ = false;
    if (options.raw && !options.loopback) {
        ComPtr<IAudioClient2> client2;
        if (SUCCEEDED(client.As(&client2))) {
            AudioClientProperties props{};
            props.cbSize = sizeof(props);
            props.bIsOffload = FALSE;
            props.eCategory = AudioCategory_Other;
            props.Options = AUDCLNT_STREAMOPTIONS_RAW;
            rawApplied_ = SUCCEEDED(client2->SetClientProperties(&props));
        }
    }

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) {
        error = "GetMixFormat: " + hresultToString(hr);
        return false;
    }
    const uint32_t mixChannels = mix->nChannels;
    CoTaskMemFree(mix);

    const DWORD baseFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                            (options.loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
    const REFERENCE_TIME duration = REFERENCE_TIME(options.bufferMs) * kRefTimesPerMs;

    // Порядок попыток: запрошенные каналы + event, каналы микса + event,
    // затем то же без event (polling). Каждая неудача пересоздаёт клиент.
    struct Attempt {
        uint32_t channels;
        bool event;
    };
    const Attempt attempts[] = {
        {options.channels, true}, {mixChannels, true}, {options.channels, false}, {mixChannels, false}};
    std::string lastErr;
    bool ok = false;
    for (const Attempt& a : attempts) {
        if (a.channels == 0) continue;
        if (&a != &attempts[0]) {
            client.Reset();
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
            if (FAILED(hr)) continue;
            if (rawApplied_) {
                ComPtr<IAudioClient2> client2;
                if (SUCCEEDED(client.As(&client2))) {
                    AudioClientProperties props{};
                    props.cbSize = sizeof(props);
                    props.eCategory = AudioCategory_Other;
                    props.Options = AUDCLNT_STREAMOPTIONS_RAW;
                    client2->SetClientProperties(&props);
                }
            }
        }
        WAVEFORMATEXTENSIBLE want = makeFloatFormat(options.sampleRate, a.channels);
        const DWORD flags = baseFlags | (a.event ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, &want.Format, nullptr);
        if (SUCCEEDED(hr)) {
            deviceChannels_ = a.channels;
            eventDriven_ = a.event;
            ok = true;
            break;
        }
        lastErr = hresultToString(hr);
    }
    if (!ok) {
        error = "IAudioClient::Initialize: " + lastErr;
        return false;
    }

    if (eventDriven_) {
        event_ = Handle(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        hr = client->SetEventHandle(event_.get());
        if (FAILED(hr)) {
            error = "SetEventHandle: " + hresultToString(hr);
            return false;
        }
    }
    hr = client->GetBufferSize(&bufferFrames_);
    if (FAILED(hr)) {
        error = "GetBufferSize: " + hresultToString(hr);
        return false;
    }
    hr = client->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) {
        error = "IAudioCaptureClient: " + hresultToString(hr);
        return false;
    }
    client_ = client;
    stopEvent_ = Handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    scratch_.assign(size_t(bufferFrames_) * options.channels, 0.0f);
    threadError_.clear();
    hasError_.store(false);
    return true;
}

bool CaptureStream::start(std::string& error) {
    if (!client_) {
        error = "capture stream not opened";
        return false;
    }
    if (running_.load()) return true;
    ResetEvent(stopEvent_.get());
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        error = "IAudioClient::Start: " + hresultToString(hr);
        return false;
    }
    running_.store(true);
    thread_ = std::thread([this] { threadMain(); });
    return true;
}

void CaptureStream::stop() {
    if (!running_.load()) return;
    running_.store(false);
    SetEvent(stopEvent_.get());
    if (thread_.joinable()) thread_.join();
    if (client_) client_->Stop();
}

void CaptureStream::close() {
    stop();
    capture_.Reset();
    client_.Reset();
    event_.reset();
    stopEvent_.reset();
}

std::string CaptureStream::lastError() const {
    return hasError_.load() ? threadError_ : std::string();
}

void CaptureStream::deliver(const BYTE* data, uint32_t frames, DWORD flags, uint64_t qpc) {
    const uint32_t out = options_.channels;
    const uint32_t in = deviceChannels_;
    const float* src = reinterpret_cast<const float*>(data);
    float* dst = scratch_.data();
    const size_t need = size_t(frames) * out;
    if (need > scratch_.size()) {
        // Больше буфера WASAPI быть не должно; на всякий случай режем.
        frames = uint32_t(scratch_.size() / out);
    }

    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        std::memset(dst, 0, size_t(frames) * out * sizeof(float));
    } else if (in == out) {
        std::memcpy(dst, src, size_t(frames) * out * sizeof(float));
    } else if (out == 1) {
        // Downmix: среднее всех каналов.
        const float k = 1.0f / float(in);
        for (uint32_t i = 0; i < frames; ++i) {
            float s = 0.0f;
            for (uint32_t c = 0; c < in; ++c) s += src[size_t(i) * in + c];
            dst[i] = s * k;
        }
    } else {
        // Берём первые out каналов, недостающие дублируем последним.
        for (uint32_t i = 0; i < frames; ++i) {
            for (uint32_t c = 0; c < out; ++c) {
                dst[size_t(i) * out + c] = src[size_t(i) * in + std::min(c, in - 1)];
            }
        }
    }

    CapturePacket p;
    p.interleaved = dst;
    p.frames = frames;
    p.qpc100ns = int64_t(qpc);
    p.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
    p.timestampError = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
    handler_(p);
}

void CaptureStream::threadMain() {
    ComInit com;
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    HANDLE waits[2] = {stopEvent_.get(), event_.get()};
    const DWORD waitCount = eventDriven_ ? 2 : 1;
    // Polling-режим: опрашиваем каждые ~5 ms.
    const DWORD timeout = eventDriven_ ? 2000 : 5;

    while (running_.load()) {
        const DWORD w = WaitForMultipleObjects(waitCount, waits, FALSE, timeout);
        if (w == WAIT_OBJECT_0) break;  // stop
        if (w == WAIT_FAILED) {
            threadError_ = "WaitForMultipleObjects failed";
            hasError_.store(true);
            break;
        }
        // WAIT_TIMEOUT в event-режиме: устройство молчит (возможно, loopback без
        // render-потоков); просто ждём дальше. Пропуски заполнит PacketAssembler.
        for (;;) {
            UINT32 next = 0;
            HRESULT hr = capture_->GetNextPacketSize(&next);
            if (FAILED(hr)) {
                threadError_ = "GetNextPacketSize: " + hresultToString(hr);
                hasError_.store(true);
                running_.store(false);
                break;
            }
            if (next == 0) break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devPos = 0, qpc = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, &devPos, &qpc);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(hr)) {
                threadError_ = "GetBuffer: " + hresultToString(hr);
                hasError_.store(true);
                running_.store(false);
                break;
            }
            if (frames > 0) deliver(data, frames, flags, qpc);
            capture_->ReleaseBuffer(frames);
        }
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}

}  // namespace bomboec::wasapi
