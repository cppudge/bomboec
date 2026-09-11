#pragma once

#include "core/config.h"
#include "engine/pipeline.h"
#include "wasapi/capture_stream.h"
#include "wasapi/render_stream.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace bomboec {

// Состояние для окна статуса: счётчики конвейера плюс устройства и потоки.
struct EngineStatus : PipelineStats {
    bool running = false;
    std::string error;  // фатальная ошибка потоков, если была
    std::string micName, speakersName, outputName;
    bool micRaw = false;
    bool micEventDriven = false, refEventDriven = false;    // иначе polling
    uint32_t micDeviceChannels = 0, refDeviceChannels = 0;  // сколько каналов реально отдаёт engine
    bool mmcss = false;                                     // все потоки получили MMCSS "Pro Audio"
    StageStats stats;
    double referenceLeadMs = 0.0;
    uint32_t outRenderMs = 0;  // целевое заполнение буфера WASAPI выхода
};

// Pipeline, подключённый к устройствам: микрофон и loopback колонок
// захватываются через WASAPI, результат уходит в render endpoint (virtual
// cable), на колонках играет keepalive-тишина, без которой loopback молчит.
// Выравнивание, DSP и регулятор выхода живут в Pipeline.
//
// Потоки: loopback capture, mic capture (весь DSP), render выхода, keepalive.
// Буфер WASAPI выхода дозаполняется только до output_render_ms.
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // При ошибке всё уже открытое закрыто, error объясняет причину.
    bool start(const AppConfig& cfg, std::string& error);
    void stop();
    bool running() const { return running_.load(); }
    EngineStatus status() const;

private:
    bool open(const AppConfig& cfg, std::string& error);

    // Pipeline объявлен раньше потоков: колбэки потоков ссылаются на него, поэтому
    // разрушается он последним.
    Pipeline pipeline_;
    wasapi::CaptureStream micStream_;
    wasapi::CaptureStream refStream_;
    wasapi::RenderStream keepalive_;
    wasapi::RenderStream output_;

    std::atomic<bool> running_{false};
    mutable std::mutex infoMutex_;
    EngineStatus info_;  // статические поля (имена устройств и т.п.)
};

}  // namespace bomboec
