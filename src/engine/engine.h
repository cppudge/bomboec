#pragma once

#include "core/config.h"
#include "engine/pipeline.h"
#include "wasapi/capture_stream.h"
#include "wasapi/devices.h"
#include "wasapi/render_stream.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace bomboec {

// Состояние для окна статуса: счётчики конвейера плюс устройства и потоки.
struct EngineStatus : PipelineStats {
    bool running = false;
    std::string error;             // фатальная ошибка потока микрофона или выхода, если была
    std::string warning;           // движок работает, но не так, как задумано: конфиг или reference
    bool referenceActive = false;  // loopback колонок открыт и работает (иначе AEC без reference)
    std::string micName, speakersName, outputName;
    std::string micId, speakersId, outputId;  // фактические endpoint id (пустой id конфига раскрыт)
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
// Обязательны только микрофон и выход. Reference (колонки) необязателен: если
// устройства нет или его поток упал, цепочка работает без reference (AEC3 с
// нулевым reference ничего не портит), статус несёт warning, а приложение
// периодически зовёт reopenReference(). Так пропавшие наушники не оставляют
// Discord без микрофона.
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

    // Из потока приложения, пока движок работает: закрыть и заново открыть loopback и
    // keepalive по устройству из конфига. false и error, если колонок всё ещё нет.
    bool reopenReference(std::string& error);

private:
    bool open(const AppConfig& cfg, std::string& error);
    // Открывает и запускает loopback + keepalive на колонках cfg_; при неудаче они
    // закрыты, warning объясняет причину, движок продолжает без reference.
    bool openReference(std::string& warning);
    void closeReference();
    void setReferenceWarning(const std::string& text);

    // Pipeline объявлен раньше потоков: колбэки потоков ссылаются на него, поэтому
    // разрушается он последним.
    Pipeline pipeline_;
    wasapi::CaptureStream micStream_;
    wasapi::CaptureStream refStream_;
    wasapi::RenderStream keepalive_;
    wasapi::RenderStream output_;

    AppConfig cfg_;  // для reopenReference
    wasapi::DeviceInfo micInfo_, outInfo_;
    std::atomic<bool> running_{false};
    std::atomic<bool> refActive_{false};
    mutable std::mutex infoMutex_;
    EngineStatus info_;          // статические поля (имена устройств и т.п.)
    std::string refWarning_;     // под infoMutex_: почему reference не работает
    std::string configWarning_;  // под infoMutex_: замечания к конфигу
};

}  // namespace bomboec
