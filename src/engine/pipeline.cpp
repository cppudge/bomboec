#include "engine/pipeline.h"

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
    frames_ = refMissing_ = refJumps_ = outUnderruns_ = outOverruns_ = outInserted_ = outDropped_ = 0;

    if (!settings_.recordDir.empty()) {
        const std::filesystem::path dir = pathFromUtf8(settings_.recordDir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        recording_ = recMic_.open(dir / "mic_raw.wav", fmt_.micChannels, rate, error) &&
                     recRef_.open(dir / "ref.wav", fmt_.referenceChannels, rate, error) &&
                     recOut_.open(dir / "out.wav", fmt_.micChannels, rate, error);
        if (!recording_) return false;
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
    if (recording_) {
        recMic_.close();
        recRef_.close();
        recOut_.close();
        recording_ = false;
    }
    chain_.reset();
}

void Pipeline::onRefPacket(const float* interleaved, uint32_t frames, int64_t ticks, PacketFlags flags) {
    refAsm_.push(interleaved, frames, ticks, flags);
}

void Pipeline::onMicPacket(const float* interleaved, uint32_t frames, int64_t ticks, PacketFlags flags) {
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
        if (recording_) {
            recMic_.write(micBuf_.data(), frame);
            recRef_.write(refBuf_.data(), frame);
        }

        if (chain_) chain_->process(micFrame_, &refFrame_);

        outDb_.store(rmsDb(micFrame_.planes()[0], frame), std::memory_order_relaxed);
        if (recording_) {
            micFrame_.toInterleaved(micBuf_.data());
            recOut_.write(micBuf_.data(), frame);
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
    const uint32_t n = outRing_.read(interleaved, frames);
    if (n < frames) {
        std::memset(interleaved + size_t(n) * settings_.outputChannels, 0,
                    size_t(frames - n) * settings_.outputChannels * sizeof(float));
        outUnderruns_.fetch_add(1, std::memory_order_relaxed);
    }
    // Грубый сброс, если накопилось больше полусекунды сверх цели (например,
    // после долгого стопа render-потока): плавный регулятор такое рассасывал
    // бы минутами.
    uint32_t left = outRing_.readable();
    const uint32_t hardLimit = fill_.target() + fmt_.sampleRate / 2;
    if (left > hardLimit) {
        outRing_.discard(left - fill_.target());
        outOverruns_.fetch_add(1, std::memory_order_relaxed);
        left = outRing_.readable();
    }
    fill_.observe(left);
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
    const uint32_t margin = fill_.marginFrames();
    s.outMarginMs = margin == FillController::kNone ? 0 : margin * 1000 / fmt_.sampleRate;
    s.micGaps = micAsm_.stats().gaps;
    s.refGaps = refAsm_.stats().gaps;
    s.micResyncs = micAsm_.stats().resyncs;
    s.refResyncs = refAsm_.stats().resyncs;
    s.micDriftPpm = micAsm_.timelineSnapshot().driftPpm();
    s.refDriftPpm = refAsm_.timelineSnapshot().driftPpm();
    s.outBufferedMs = outRing_.readable() * 1000 / fmt_.sampleRate;
    return s;
}

StageStats Pipeline::chainStats() const { return chain_ ? chain_->stats() : StageStats{}; }

}  // namespace bomboec
