#pragma once

#include "wasapi/com_util.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace bomboec::wasapi {

struct CapturePacket {
    const float* interleaved = nullptr;  // channels() каналов, float32
    uint32_t frames = 0;
    int64_t qpc100ns = 0;                // время первого сэмпла пакета
    bool discontinuity = false;          // AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY
    bool timestampError = false;         // AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR
};

// Захват с endpoint'а (микрофон или loopback с render-устройства) в shared
// event-driven режиме. Формат запрашивается как float32 sampleRate/channels,
// конвертацию делает audio engine (AUTOCONVERTPCM); если engine отказывает
// в числе каналов, каналы сводятся программно.
//
// Колбэк вызывается из внутреннего потока (MMCSS "Pro Audio"); он не должен
// блокироваться и аллоцировать.
class CaptureStream {
public:
    struct Options {
        bool loopback = false;      // захват с render endpoint'а
        bool raw = false;           // AUDCLNT_STREAMOPTIONS_RAW: без APO endpoint'а
        uint32_t sampleRate = 48000;
        uint32_t channels = 1;
        uint32_t bufferMs = 200;    // ёмкость буфера WASAPI, не задержка
    };
    using PacketHandler = std::function<void(const CapturePacket&)>;

    CaptureStream() = default;
    ~CaptureStream();
    CaptureStream(const CaptureStream&) = delete;
    CaptureStream& operator=(const CaptureStream&) = delete;

    bool open(IMMDevice* device, const Options& options, PacketHandler handler, std::string& error);
    bool start(std::string& error);
    void stop();
    void close();

    uint32_t channels() const { return options_.channels; }
    uint32_t sampleRate() const { return options_.sampleRate; }
    uint32_t deviceChannels() const { return deviceChannels_; }  // что реально отдаёт engine
    bool rawApplied() const { return rawApplied_; }
    bool eventDriven() const { return eventDriven_; }
    uint32_t bufferFrames() const { return bufferFrames_; }
    // Фатальная ошибка потока (устройство пропало и т.п.); пусто, если всё хорошо.
    std::string lastError() const;

private:
    void threadMain();
    void deliver(const BYTE* data, uint32_t frames, DWORD flags, uint64_t qpc);

    Options options_;
    PacketHandler handler_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    Handle event_;
    Handle stopEvent_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    uint32_t deviceChannels_ = 0;
    uint32_t bufferFrames_ = 0;
    bool rawApplied_ = false;
    bool eventDriven_ = true;
    std::vector<float> scratch_;
    mutable std::atomic<bool> hasError_{false};
    std::string threadError_;
};

}  // namespace bomboec::wasapi
