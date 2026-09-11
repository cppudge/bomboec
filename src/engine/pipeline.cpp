#include "engine/pipeline.h"

#include "core/denormals.h"
#include "core/utf8.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace bomboec {

namespace {

constexpr double kTps = 1e7;    // метки WASAPI: 100 ns
constexpr int kMaxStretch = 4;  // максимум сэмплов коррекции на кадр (0.8 % на 480)

// Выравнивание reference (referencePosition).
constexpr double kPull = 0.01;            // доля ошибки предсказания, на которую позиция подтягивается за кадр
constexpr double kSlipHysteresis = 0.75;  // целый индекс сдвигается, когда позиция ушла от него на столько
constexpr double kRelockSamples = 480.0;  // расхождение больше 10 ms: позиция переустанавливается

// Предел задержки выхода (fillOutput).
constexpr uint32_t kSoftExcessMs = 30;   // устойчивый излишек сверх цели, после которого выбрасываем
constexpr uint32_t kHardExcessMs = 500;  // излишек, который выбрасываем сразу, не дожидаясь окна

constexpr uint32_t kChainStatsEveryFrames = 10;  // снимок статистики стадий для окна статуса: раз в 100 ms

// Дорожки debug-записи (порядок как в configure).
constexpr size_t kRecMic = 0, kRecRef = 1, kRecOut = 2;

float rmsDb(const float* x, uint32_t n) {
    double e = 0.0;
    for (uint32_t i = 0; i < n; ++i) e += double(x[i]) * x[i];
    return float(10.0 * std::log10(e / std::max<uint32_t>(n, 1) + 1e-12));
}

}  // namespace

Pipeline::Pipeline() = default;
Pipeline::~Pipeline() = default;

bool Pipeline::configure(const PipelineFormat& fmt, const EngineSettings& settings, std::unique_ptr<Chain> chain,
                         std::string& error) {
    reset();
    fmt_ = fmt;
    settings_ = settings;
    chain_ = std::move(chain);

    const uint32_t rate = fmt_.sampleRate;
    micRing_.resize(fmt_.micChannels, rate * 2);
    refRing_.resize(fmt_.referenceChannels, rate * 5);
    outRing_.resize(settings_.outputChannels, rate * 2);
    micAsm_.configure(rate, kTps, &micRing_);
    refAsm_.configure(rate, kTps, &refRing_);
    micBuf_.assign(size_t(fmt_.frameSamples) * fmt_.micChannels, 0.0f);
    refBuf_.assign(size_t(fmt_.frameSamples) * fmt_.referenceChannels, 0.0f);
    outBuf_.assign((size_t(fmt_.frameSamples) + kMaxStretch) * settings_.outputChannels, 0.0f);
    stretchBuf_.assign(size_t(fmt_.frameSamples) + kMaxStretch, 0.0f);
    micFrame_.resize(fmt_.micChannels, fmt_.frameSamples);
    refFrame_.resize(fmt_.referenceChannels, fmt_.frameSamples);
    leadTicks_ = int64_t(settings_.referenceLeadMs) * 10'000;
    refKeepFrames_ = rate / 2;  // держим 500 ms истории reference позади точки чтения
    refLocked_ = false;
    underrunsSeen_ = 0;
    softExcessFrames_ = rate * kSoftExcessMs / 1000;
    fadeBuf_.assign(size_t(rate / 1000) * settings_.outputChannels, 0.0f);  // кроссфейд 1 ms
    fadePending_ = false;
    chainStats_.store({});
    chainStatsFrames_ = 0;
    frames_ = refMissing_ = refJumps_ = outUnderruns_ = outOverruns_ = outInserted_ = outDropped_ = outTrimmed_ = 0;

    if (!settings_.recordDir.empty()) {
        const std::vector<Recorder::Track> tracks = {
            {"mic_raw.wav", fmt_.micChannels}, {"ref.wav", fmt_.referenceChannels}, {"out.wav", fmt_.micChannels}};
        if (!recorder_.open(pathFromUtf8(settings_.recordDir), tracks, rate, error)) return false;
    }

    // Запас в выходном кольце против джиттера mic-потока: FillController держит
    // минимальный остаток после чтения около outputBufferMs. Предзаполняем
    // тишиной ровно на запас: render-поток начинает читать кольцо только после
    // первого кадра микрофона (см. fillOutput), так что стартуем сразу в цели.
    const uint32_t marginFrames = rate * settings_.outputBufferMs / 1000;
    fill_.configure(marginFrames, /*windowReads=*/rate / fmt_.frameSamples, 1.0 / 1200.0, kMaxStretch);
    outRing_.writeSilence(marginFrames);
    return true;
}

void Pipeline::reset() {
    recorder_.close();
    chain_.reset();
}

void Pipeline::onRefPacket(const float* interleaved, uint32_t frames, int64_t ticks, PacketFlags flags) {
    refAsm_.push(interleaved, frames, ticks, flags);
}

void Pipeline::onMicPacket(const float* interleaved, uint32_t frames, int64_t ticks, PacketFlags flags) {
    const ScopedFlushDenormals denormals;
    micAsm_.push(interleaved, frames, ticks, flags);
    processAvailable();
}

void Pipeline::processAvailable() {
    const uint32_t frame = fmt_.frameSamples;
    while (micRing_.readable() >= frame) {
        const uint64_t micIndex = micRing_.totalRead();
        micRing_.read(micBuf_.data(), frame);
        micFrame_.fromInterleaved(micBuf_.data());

        // Reference по таймлайну.
        bool haveRef = false;
        if (refAsm_.started()) {
            const Timeline mt = micAsm_.timelineSnapshot();
            const Timeline rt = refAsm_.timelineSnapshot();
            const double predicted = rt.sampleAt(mt.ticksAt(double(micIndex)) - leadTicks_);
            const int64_t refIndex = referencePosition(predicted, rt.estimatedRate() / mt.estimatedRate());
            if (refIndex >= 0) {  // до начала reference (первые кадры после старта) читать нечего
                const RingBuffer::ReadAtResult r = refRing_.readAt(uint64_t(refIndex), refBuf_.data(), frame);
                haveRef = (r == RingBuffer::ReadAtResult::Ok);
                // Отбрасываем историю старше refKeepFrames_ позади точки чтения.
                const uint64_t keepFrom = uint64_t(refIndex) > refKeepFrames_ ? uint64_t(refIndex) - refKeepFrames_ : 0;
                const uint64_t consumed = refRing_.totalRead();
                if (keepFrom > consumed) refRing_.discard(uint32_t(keepFrom - consumed));
            }
        }
        if (!haveRef) {
            refMissing_.fetch_add(1, std::memory_order_relaxed);
            std::memset(refBuf_.data(), 0, refBuf_.size() * sizeof(float));
        }
        refFrame_.fromInterleaved(refBuf_.data());

        micDb_.store(rmsDb(micFrame_.planes()[0], frame), std::memory_order_relaxed);
        refDb_.store(rmsDb(refFrame_.planes()[0], frame), std::memory_order_relaxed);
        if (recorder_.isOpen()) {
            recorder_.push(kRecMic, micBuf_.data(), frame);
            recorder_.push(kRecRef, refBuf_.data(), frame);
        }

        if (chain_) {
            chain_->process(micFrame_, &refFrame_);
            if (++chainStatsFrames_ >= kChainStatsEveryFrames) {
                chainStatsFrames_ = 0;
                chainStats_.store(chain_->stats());
            }
        }

        outDb_.store(rmsDb(micFrame_.planes()[0], frame), std::memory_order_relaxed);
        if (recorder_.isOpen()) {
            micFrame_.toInterleaved(micBuf_.data());
            recorder_.push(kRecOut, micBuf_.data(), frame);
        }

        // Регулятор заполнения: кадр растягивается/сжимается на d сэмплов,
        // чтобы запас в outRing не рос от дрейфа mic относительно выхода.
        const float* mono = micFrame_.planes()[0];
        uint32_t outFrames = frame;
        const int d = fill_.step();
        if (d != 0) {
            outFrames = uint32_t(int(frame) + d);
            stretchLinear(mono, frame, stretchBuf_.data(), outFrames);
            mono = stretchBuf_.data();
            if (d > 0) outInserted_.fetch_add(uint64_t(d), std::memory_order_relaxed);
            else outDropped_.fetch_add(uint64_t(-d), std::memory_order_relaxed);
        }

        // outRing опустел (микрофон стоял): запас восстанавливается тишиной сразу, а не
        // растяжением по 4 сэмпла за кадр (10 ms запаса это 1.2 с искажённого звука).
        const uint64_t underruns = outUnderruns_.load(std::memory_order_relaxed);
        if (underruns != underrunsSeen_) {
            underrunsSeen_ = underruns;
            const uint32_t have = outRing_.readable();
            if (have < fill_.target()) outRing_.writeSilence(fill_.target() - have);
        }

        // Моно -> каналы выхода дублированием.
        const uint32_t oc = settings_.outputChannels;
        for (uint32_t i = 0; i < outFrames; ++i) {
            for (uint32_t c = 0; c < oc; ++c) outBuf_[size_t(i) * oc + c] = mono[i];
        }
        if (outRing_.write(outBuf_.data(), outFrames) < outFrames) {
            outOverruns_.fetch_add(1, std::memory_order_relaxed);
        }
        frames_.fetch_add(1, std::memory_order_relaxed);
    }
}

// Предсказание по таймлайнам несёт джиттер меток обоих потоков: у Yeti до ±10
// сэмплов на кадр. Если читать прямо по нему, каждый кадр повторяет или пропускает
// кусок reference, и для адаптивного фильтра AEC3 эхо-тракт всё время дёргается
// (в симуляции с джиттером как на машине разработки подавление падало до 0.6 dB).
// Поэтому позиция ведётся отдельно:
//  - за кадр она сдвигается на frame * ratio: дрейф часов учитывается сразу;
//  - к предсказанию подтягивается доля kPull ошибки: джиттер усредняется;
//  - целый индекс чтения меняется на ±1 сэмпл, только когда позиция ушла от него
//    дальше kSlipHysteresis: проскальзывания редкие (при 140 ppm раз в ~15 кадров)
//    и без дребезга на границе;
//  - расхождение больше kRelockSamples (старт, ресинхронизация таймлайна) - сразу
//    к предсказанию.
int64_t Pipeline::referencePosition(double predicted, double ratio) {
    const uint32_t frame = fmt_.frameSamples;
    if (refLocked_) {
        refPos_ += double(frame) * ratio;
        refIndex_ += int64_t(frame);
        const double err = predicted - refPos_;
        if (std::abs(err) < kRelockSamples) {
            refPos_ += kPull * err;
            const double offset = refPos_ - double(refIndex_);
            if (offset > kSlipHysteresis) ++refIndex_;
            else if (offset < -kSlipHysteresis) --refIndex_;
            if (std::abs(offset) > kSlipHysteresis) refJumps_.fetch_add(1, std::memory_order_relaxed);
            return refIndex_;
        }
        refJumps_.fetch_add(1, std::memory_order_relaxed);
    }
    refLocked_ = true;
    refPos_ = predicted;
    refIndex_ = std::llround(predicted);
    return refIndex_;
}

void Pipeline::fillOutput(float* interleaved, uint32_t frames) {
    // Пока микрофон не дал ни одного кадра, отдаём тишину, не трогая
    // предзаполнение: иначе render-поток съедает его до старта mic-потока и
    // регулятор потом десятки секунд поднимает запас с нуля.
    if (frames_.load(std::memory_order_relaxed) == 0) {
        std::memset(interleaved, 0, size_t(frames) * settings_.outputChannels * sizeof(float));
        return;
    }
    const uint32_t oc = settings_.outputChannels;

    // Излишек задержки, который регулятор рассасывал бы по 4 сэмпла на кадр (200 ms
    // за 40 с): устойчивый, то есть минимальный остаток за полное окно измерений
    // выше цели больше чем на kSoftExcessMs (микрофон отдал накопленное после
    // подвисания), или больше kHardExcessMs прямо сейчас (долго стоял render-поток).
    const uint32_t target = fill_.target();
    const uint32_t avail = outRing_.readable();
    uint32_t excess = 0;
    if (fill_.fullWindow() && fill_.marginFrames() > target + softExcessFrames_) {
        excess = fill_.marginFrames() - target;
    }
    const uint32_t hardLimit = frames + target + fmt_.sampleRate * kHardExcessMs / 1000;
    if (avail > hardLimit) excess = std::max(excess, avail - frames - target);
    if (excess > 0) trimOutput(excess);

    const uint32_t n = outRing_.read(interleaved, frames);
    if (fadePending_) {
        const uint32_t k = std::min(fadeFrames_, n);
        for (uint32_t i = 0; i < k; ++i) {
            const float w = float(i + 1) / float(k + 1);
            for (uint32_t c = 0; c < oc; ++c) {
                float& x = interleaved[size_t(i) * oc + c];
                x = fadeBuf_[size_t(i) * oc + c] * (1.0f - w) + x * w;
            }
        }
        fadePending_ = false;
    }
    if (n < frames) {
        std::memset(interleaved + size_t(n) * oc, 0, size_t(frames - n) * oc * sizeof(float));
        outUnderruns_.fetch_add(1, std::memory_order_relaxed);
        // Прежние измерения окна больше не про это кольцо; запас вернёт mic-поток
        // (processAvailable), регулятору не нужно наращивать его растяжением.
        fill_.restartWindow();
        return;
    }
    fill_.observe(outRing_.readable());
}

void Pipeline::trimOutput(uint32_t drop) {
    fadeFrames_ = outRing_.peek(fadeBuf_.data(), uint32_t(fadeBuf_.size() / settings_.outputChannels));
    fadePending_ = fadeFrames_ > 0;
    outRing_.discard(drop);
    outTrimmed_.fetch_add(drop, std::memory_order_relaxed);
    fill_.restartWindow();
}

PipelineStats Pipeline::stats() const {
    PipelineStats s;
    s.micDb = micDb_.load();
    s.refDb = refDb_.load();
    s.outDb = outDb_.load();
    s.framesProcessed = frames_.load();
    s.refMissing = refMissing_.load();
    s.refJumps = refJumps_.load();
    s.outUnderruns = outUnderruns_.load();
    s.outOverruns = outOverruns_.load();
    s.outInserted = outInserted_.load();
    s.outDropped = outDropped_.load();
    s.outTrimmed = outTrimmed_.load();
    s.recordDropped = recorder_.dropped();
    const uint32_t margin = fill_.marginFrames();
    s.outMarginMs = margin == FillController::kNone ? 0 : margin * 1000 / fmt_.sampleRate;
    const PacketAssembler::Stats mic = micAsm_.statsSnapshot();
    const PacketAssembler::Stats ref = refAsm_.statsSnapshot();
    s.micGaps = mic.gaps;
    s.refGaps = ref.gaps;
    s.micResyncs = mic.resyncs;
    s.refResyncs = ref.resyncs;
    s.micDriftPpm = micAsm_.timelineSnapshot().driftPpm();
    s.refDriftPpm = refAsm_.timelineSnapshot().driftPpm();
    s.outBufferedMs = outRing_.readable() * 1000 / fmt_.sampleRate;
    return s;
}

StageStats Pipeline::chainStats() const { return chainStats_.load(); }

}  // namespace bomboec
