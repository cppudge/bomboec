#pragma once

#include "core/chain.h"
#include "core/config.h"
#include "core/fill_controller.h"
#include "core/frame.h"
#include "core/packet_assembler.h"
#include "core/ring_buffer.h"
#include "core/wav.h"
#include "wasapi/capture_stream.h"
#include "wasapi/render_stream.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bomboec {

struct EngineStatus {
    bool running = false;
    std::string error;  // фатальная ошибка потоков, если была
    std::string micName, speakersName, outputName;
    bool micRaw = false;
    float micDb = -100.0f, refDb = -100.0f, outDb = -100.0f;  // уровни последнего кадра
    StageStats stats;
    uint64_t framesProcessed = 0;
    uint64_t refMissing = 0;  // кадров, где reference не был доступен целиком
    uint64_t micGaps = 0, refGaps = 0;
    uint64_t outUnderruns = 0, outOverruns = 0;
    uint64_t outInserted = 0, outDropped = 0;  // сэмплов добавлено/убрано регулятором заполнения
    double micDriftPpm = 0.0, refDriftPpm = 0.0;
    double referenceLeadMs = 0.0;
    uint32_t outBufferedMs = 0;  // сколько сейчас в выходном ring
    uint32_t outMarginMs = 0;    // минимальный остаток после чтения за окно (цель: output_buffer_ms)
    uint32_t outRenderMs = 0;    // целевое заполнение буфера WASAPI выхода
};

// Realtime-конвейер: mic -> [выровненный reference] -> Chain -> render endpoint.
//
// Потоки: loopback capture (producer refRing), mic capture (producer micRing и
// весь DSP), render (consumer outRing), keepalive-тишина на колонках.
// Reference для кадра mic с временем t берётся из refRing по индексу
// refTimeline.sampleAt(t - lead): дрейф часов компенсируется проскальзыванием
// на сэмпл, а не накоплением задержки.
//
// Задержка выхода: outRing держит запас output_buffer_ms после каждого чтения
// (FillController подгоняет его растяжением кадра на 1..4 сэмпла, компенсируя
// дрейф микрофона относительно выходного устройства), буфер WASAPI выхода
// дозаполняется только до output_render_ms.
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool start(const AppConfig& cfg, std::string& error);
    void stop();
    bool running() const { return running_.load(); }
    EngineStatus status() const;

private:
    void onMicPacket(const wasapi::CapturePacket& p);
    void onRefPacket(const wasapi::CapturePacket& p);
    void fillOutput(float* interleaved, uint32_t frames);
    void processAvailable();

    PipelineFormat fmt_;
    EngineSettings settings_;
    std::unique_ptr<Chain> chain_;

    wasapi::CaptureStream micStream_;
    wasapi::CaptureStream refStream_;
    wasapi::RenderStream keepalive_;
    wasapi::RenderStream output_;

    RingBuffer micRing_, refRing_, outRing_;
    PacketAssembler micAsm_, refAsm_;
    FillController fill_;
    std::vector<float> micBuf_, refBuf_, outBuf_, stretchBuf_;
    Frame micFrame_, refFrame_;
    int64_t leadTicks_ = 0;
    uint64_t refKeepFrames_ = 0;

    // debug-запись
    WavWriter recMic_, recRef_, recOut_;
    bool recording_ = false;

    std::atomic<bool> running_{false};
    std::atomic<float> micDb_{-100.0f}, refDb_{-100.0f}, outDb_{-100.0f};
    std::atomic<uint64_t> frames_{0}, refMissing_{0}, outUnderruns_{0}, outOverruns_{0};
    std::atomic<uint64_t> outInserted_{0}, outDropped_{0};
    mutable std::mutex infoMutex_;
    EngineStatus info_;  // статические поля (имена устройств и т.п.)
};

}  // namespace bomboec
