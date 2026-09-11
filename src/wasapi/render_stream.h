#pragma once

#include "wasapi/com_util.h"

#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace bomboec::wasapi {

// Воспроизведение float32 на render endpoint в shared event-driven режиме.
// Используется как keepalive-поток тишины для loopback и как выход в
// virtual cable. Колбэк заполняет interleaved-буфер из внутреннего потока
// (MMCSS "Pro Audio") и не должен блокироваться; nullptr = тишина.
class RenderStream {
public:
    struct Options {
        uint32_t sampleRate = 48000;
        uint32_t channels = 2;
        uint32_t bufferMs = 40;  // ёмкость буфера WASAPI, не задержка
        // До скольких ms дозаполнять буфер на каждое событие; 0 = до краёв.
        // Задержка на этом участке равна целевому заполнению; безопасный
        // минимум 2 периода engine (обычно 20 ms), меньше периода не даём.
        uint32_t targetMs = 0;
    };
    using FillHandler = std::function<void(float* interleaved, uint32_t frames)>;

    RenderStream() = default;
    ~RenderStream();
    RenderStream(const RenderStream&) = delete;
    RenderStream& operator=(const RenderStream&) = delete;

    bool open(IMMDevice* device, const Options& options, FillHandler handler, std::string& error);
    bool start(std::string& error);
    void stop();
    void close();

    uint32_t channels() const { return options_.channels; }
    uint32_t bufferFrames() const { return bufferFrames_; }
    uint32_t periodFrames() const { return periodFrames_; }  // период engine
    uint32_t targetFrames() const { return targetFrames_; }  // целевое заполнение
    bool mmcssApplied() const { return mmcss_.load(); }
    // Фатальная ошибка потока; пусто, если всё хорошо. Поток при ошибке
    // завершается сам, stop()/close() его собирают.
    std::string lastError() const;

private:
    void threadMain();
    // Дозаполняет буфер до цели; false при фатальной ошибке устройства.
    bool fillOnce();
    void fail(std::string text);

    Options options_;
    FillHandler handler_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    Handle event_;
    Handle stopEvent_;
    std::thread thread_;
    std::atomic<bool> running_{false};  // сброс просит поток остановиться
    std::atomic<bool> mmcss_{false};
    uint32_t bufferFrames_ = 0;
    uint32_t periodFrames_ = 0;
    uint32_t targetFrames_ = 0;
    // threadError_ пишет только поток и до hasError_ = true; после этого строка не меняется.
    std::atomic<bool> hasError_{false};
    std::string threadError_;
};

}  // namespace bomboec::wasapi
