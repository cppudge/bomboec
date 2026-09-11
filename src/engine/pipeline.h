#pragma once

#include "core/audio_format.h"
#include "core/chain.h"
#include "core/config.h"
#include "core/fill_controller.h"
#include "core/frame.h"
#include "core/packet_assembler.h"
#include "core/ring_buffer.h"
#include "core/seqlock.h"
#include "engine/recorder.h"

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
    uint64_t outTrimmed = 0;                   // сэмплов выброшено разом пределом задержки
    uint64_t recordDropped = 0;                // debug-запись: кадров не успело на диск
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
// дрейф микрофона относительно выходного устройства). Излишек, который регулятор
// рассасывал бы десятки секунд (микрофон отдал накопленное после подвисания),
// выбрасывается разом с кроссфейдом; опустевшее кольцо сразу получает запас тишиной.
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
    // Render-поток: выбросить drop кадров из outRing; следующее чтение начнётся с
    // кроссфейда из того, что прозвучало бы без выброса.
    void trimOutput(uint32_t drop);

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
    double refPos_ = 0.0;            // сглаженная дробная позиция reference для текущего кадра
    int64_t refIndex_ = 0;           // целый индекс чтения: следует за refPos_ с гистерезисом
    int relockFrames_ = 0;           // кадров подряд с расхождением больше kRelockSamples
    uint64_t underrunsSeen_ = 0;     // mic-поток: сколько опустошений outRing уже восстановлено
    uint32_t softExcessFrames_ = 0;  // излишек сверх цели, после которого выбрасываем разом
    std::vector<float> fadeBuf_;     // render-поток: начало выброшенного куска для кроссфейда
    uint32_t fadeFrames_ = 0;
    bool fadePending_ = false;

    Recorder recorder_;  // debug-запись mic_raw/ref/out: файлы пишет свой поток

    std::atomic<float> micDb_{-100.0f}, refDb_{-100.0f}, outDb_{-100.0f};
    std::atomic<uint64_t> frames_{0}, refMissing_{0}, refJumps_{0}, outUnderruns_{0}, outOverruns_{0};
    std::atomic<uint64_t> outInserted_{0}, outDropped_{0}, outTrimmed_{0};
    // Статистику стадий снимает mic-поток (stats() стадий зовётся только из него), читает любой.
    SeqLock<StageStats> chainStats_;
    uint32_t chainStatsFrames_ = 0;
};

}  // namespace bomboec
