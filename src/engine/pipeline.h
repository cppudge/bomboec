#pragma once

#include "core/audio_format.h"
#include "core/chain.h"
#include "core/config.h"
#include "core/fill_controller.h"
#include "core/frame.h"
#include "core/packet_assembler.h"
#include "core/ring_buffer.h"
#include "core/wav.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bomboec {

// Счётчики и уровни конвейера (окно статуса, bomboec-run, тесты).
struct PipelineStats {
    float micDb = -100.0f, refDb = -100.0f, outDb = -100.0f;  // уровни последнего кадра
    uint64_t framesProcessed = 0;
    uint64_t refMissing = 0;  // кадров, где reference не был доступен целиком
    uint64_t refJumps = 0;    // кадров, где чтение reference не продолжило предыдущий кадр
    uint64_t micGaps = 0, refGaps = 0;
    uint64_t micResyncs = 0, refResyncs = 0;  // PacketAssembler начинал таймлайн заново
    uint64_t outUnderruns = 0, outOverruns = 0;
    uint64_t outInserted = 0, outDropped = 0;  // сэмплов добавлено/убрано регулятором заполнения
    double micDriftPpm = 0.0, refDriftPpm = 0.0;
    uint32_t outBufferedMs = 0;  // сколько сейчас в выходном ring
    uint32_t outMarginMs = 0;    // минимальный остаток после чтения за окно (цель: output_buffer_ms)
};

// Realtime-часть движка без устройств: сборка mic и reference из пакетов с
// QPC-метками, выравнивание reference по таймлайну, цепочка стадий, регулятор
// выходного кольца. Engine подключает её к WASAPI, тесты кормят пакетами
// напрямую (tests/sim/pipeline_sim.h), поэтому дрейф, джиттер меток, пропуски
// и всплески проверяются детерминированно.
//
// Потоки: onRefPacket (loopback) пишет refRing; onMicPacket (микрофон) пишет
// micRing и выполняет весь DSP, он же пишет outRing; fillOutput (render)
// читает outRing. Reference для кадра mic с временем t читается из refRing
// непрерывно, около индекса refTimeline.sampleAt(t - lead): дрейф часов
// компенсируется редким проскальзыванием на сэмпл, джиттер меток сглаживается
// (см. referencePosition).
//
// Задержка выхода: outRing держит запас output_buffer_ms после каждого чтения
// (FillController подгоняет его растяжением кадра на 1..4 сэмпла, компенсируя
// дрейф микрофона относительно выходного устройства).
class Pipeline {
public:
    Pipeline();
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // До старта потоков. chain уже инициализирована под fmt. settings.recordDir
    // непустой: debug-запись mic_raw/ref/out в WAV.
    bool configure(const PipelineFormat& fmt, const EngineSettings& settings, std::unique_ptr<Chain> chain,
                   std::string& error);
    // После остановки потоков: закрывает запись и цепочку.
    void reset();

    void onRefPacket(const float* interleaved, uint32_t frames, int64_t ticks, PacketFlags flags = {});
    void onMicPacket(const float* interleaved, uint32_t frames, int64_t ticks, PacketFlags flags = {});
    // interleaved: settings.outputChannels каналов.
    void fillOutput(float* interleaved, uint32_t frames);

    PipelineStats stats() const;
    StageStats chainStats() const;

private:
    void processAvailable();
    // Индекс первого сэмпла reference для очередного кадра mic. predicted: индекс по
    // таймлайнам (с джиттером меток), ratio: частота reference / частота mic.
    int64_t referencePosition(double predicted, double ratio);

    PipelineFormat fmt_;
    EngineSettings settings_;
    std::unique_ptr<Chain> chain_;

    RingBuffer micRing_, refRing_, outRing_;
    PacketAssembler micAsm_, refAsm_;
    FillController fill_;
    std::vector<float> micBuf_, refBuf_, outBuf_, stretchBuf_;
    Frame micFrame_, refFrame_;
    int64_t leadTicks_ = 0;
    uint64_t refKeepFrames_ = 0;
    bool refLocked_ = false;
    double refPos_ = 0.0;   // сглаженная дробная позиция reference для текущего кадра
    int64_t refIndex_ = 0;  // целый индекс чтения: следует за refPos_ с гистерезисом

    // debug-запись
    WavWriter recMic_, recRef_, recOut_;
    bool recording_ = false;

    std::atomic<float> micDb_{-100.0f}, refDb_{-100.0f}, outDb_{-100.0f};
    std::atomic<uint64_t> frames_{0}, refMissing_{0}, refJumps_{0}, outUnderruns_{0}, outOverruns_{0};
    std::atomic<uint64_t> outInserted_{0}, outDropped_{0};
};

}  // namespace bomboec
