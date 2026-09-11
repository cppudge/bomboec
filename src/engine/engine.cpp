#include "engine/engine.h"

#include "stages/builtin_stages.h"
#include "wasapi/devices.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace bomboec {

namespace {

constexpr double kTps = 1e7;

float rmsDb(const float* x, uint32_t n) {
    double e = 0.0;
    for (uint32_t i = 0; i < n; ++i) e += double(x[i]) * x[i];
    return float(10.0 * std::log10(e / std::max<uint32_t>(n, 1) + 1e-12));
}

}  // namespace

Engine::Engine() = default;
Engine::~Engine() { stop(); }

bool Engine::start(const AppConfig& cfg, std::string& error) {
    stop();
    fmt_ = cfg.format;
    settings_ = cfg.engine;

    StageRegistry registry;
    registerBuiltinStages(registry);
    chain_ = buildChain(registry, cfg, error);
    if (!chain_ || !chain_->init(fmt_, error)) return false;

    namespace ws = wasapi;
    ws::ComPtr<IMMDevice> micDev = ws::openDevice(ws::Flow::Capture, ws::fromUtf8(settings_.micId), error);
    if (!micDev) return false;
    ws::ComPtr<IMMDevice> spkDev = ws::openDevice(ws::Flow::Render, ws::fromUtf8(settings_.speakersId), error);
    if (!spkDev) return false;
    ws::ComPtr<IMMDevice> outDev = ws::openDevice(ws::Flow::Render, ws::fromUtf8(settings_.outputId), error);
    if (!outDev) return false;
    const ws::DeviceInfo spkInfo = ws::describeDevice(spkDev.Get());
    const ws::DeviceInfo outInfo = ws::describeDevice(outDev.Get());
    if (spkInfo.id == outInfo.id) {
        // Очищенный микрофон в те же колонки = акустическая петля.
        error = "output device must differ from the reference speakers (" + ws::toUtf8(outInfo.name) + ")";
        return false;
    }
    {
        std::lock_guard<std::mutex> g(infoMutex_);
        info_ = {};
        info_.micName = ws::toUtf8(ws::describeDevice(micDev.Get()).name);
        info_.speakersName = ws::toUtf8(spkInfo.name);
        info_.outputName = ws::toUtf8(outInfo.name);
        info_.referenceLeadMs = settings_.referenceLeadMs;
    }

    const uint32_t rate = fmt_.sampleRate;
    micRing_.resize(fmt_.micChannels, rate * 2);
    refRing_.resize(fmt_.referenceChannels, rate * 5);
    outRing_.resize(settings_.outputChannels, rate * 2);
    micAsm_.configure(rate, kTps, &micRing_);
    refAsm_.configure(rate, kTps, &refRing_);
    micBuf_.assign(size_t(fmt_.frameSamples) * fmt_.micChannels, 0.0f);
    refBuf_.assign(size_t(fmt_.frameSamples) * fmt_.referenceChannels, 0.0f);
    outBuf_.assign(size_t(fmt_.frameSamples) * settings_.outputChannels, 0.0f);
    micFrame_.resize(fmt_.micChannels, fmt_.frameSamples);
    refFrame_.resize(fmt_.referenceChannels, fmt_.frameSamples);
    leadTicks_ = int64_t(settings_.referenceLeadMs) * 10'000;
    refKeepFrames_ = rate / 2;  // держим 500 ms истории reference позади точки чтения
    frames_ = refMissing_ = outUnderruns_ = outOverruns_ = 0;

    recording_ = false;
    if (!settings_.recordDir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(settings_.recordDir, ec);
        const std::filesystem::path dir = settings_.recordDir;
        recording_ = recMic_.open(dir / "mic_raw.wav", fmt_.micChannels, rate, error) &&
                     recRef_.open(dir / "ref.wav", fmt_.referenceChannels, rate, error) &&
                     recOut_.open(dir / "out.wav", fmt_.micChannels, rate, error);
        if (!recording_) return false;
    }

    ws::CaptureStream::Options micOpt;
    micOpt.channels = fmt_.micChannels;
    micOpt.raw = settings_.micRaw;
    micOpt.sampleRate = rate;
    if (!micStream_.open(micDev.Get(), micOpt, [this](const ws::CapturePacket& p) { onMicPacket(p); }, error)) {
        return false;
    }
    ws::CaptureStream::Options refOpt;
    refOpt.channels = fmt_.referenceChannels;
    refOpt.loopback = true;
    refOpt.sampleRate = rate;
    if (!refStream_.open(spkDev.Get(), refOpt, [this](const ws::CapturePacket& p) { onRefPacket(p); }, error)) {
        return false;
    }
    ws::RenderStream::Options keepOpt;
    keepOpt.sampleRate = rate;
    if (!keepalive_.open(spkDev.Get(), keepOpt, nullptr, error)) return false;

    ws::RenderStream::Options outOpt;
    outOpt.sampleRate = rate;
    outOpt.channels = settings_.outputChannels;
    outOpt.bufferMs = std::max<uint32_t>(40, settings_.outputBufferMs * 2);
    if (!output_.open(outDev.Get(), outOpt, [this](float* buf, uint32_t n) { fillOutput(buf, n); }, error)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> g(infoMutex_);
        info_.micRaw = micStream_.rawApplied();
    }

    // Предзаполнение выхода тишиной: запас против джиттера mic-потока.
    outRing_.writeSilence(rate * settings_.outputBufferMs / 1000);

    running_.store(true);
    if (!keepalive_.start(error) || !refStream_.start(error) || !micStream_.start(error) || !output_.start(error)) {
        stop();
        return false;
    }
    return true;
}

void Engine::stop() {
    running_.store(false);
    micStream_.close();
    refStream_.close();
    output_.close();
    keepalive_.close();
    if (recording_) {
        recMic_.close();
        recRef_.close();
        recOut_.close();
        recording_ = false;
    }
    chain_.reset();
}

void Engine::onRefPacket(const wasapi::CapturePacket& p) {
    refAsm_.push(p.interleaved, p.frames, p.qpc100ns);
}

void Engine::onMicPacket(const wasapi::CapturePacket& p) {
    micAsm_.push(p.interleaved, p.frames, p.qpc100ns);
    processAvailable();
}

void Engine::processAvailable() {
    const uint32_t frame = fmt_.frameSamples;
    while (running_.load(std::memory_order_relaxed) && micRing_.readable() >= frame) {
        const uint64_t micIndex = micRing_.totalRead();
        micRing_.read(micBuf_.data(), frame);
        micFrame_.fromInterleaved(micBuf_.data());

        // Reference по таймлайну.
        bool haveRef = false;
        if (refAsm_.started()) {
            const Timeline mt = micAsm_.timelineSnapshot();
            const Timeline rt = refAsm_.timelineSnapshot();
            const int64_t t = mt.ticksAt(double(micIndex)) - leadTicks_;
            const double ri = rt.sampleAt(t);
            const int64_t refIndex = ri > 0 ? int64_t(ri + 0.5) : 0;
            const RingBuffer::ReadAtResult r = refRing_.readAt(uint64_t(refIndex), refBuf_.data(), frame);
            haveRef = (r == RingBuffer::ReadAtResult::Ok);
            // Отбрасываем историю старше refKeepFrames_ позади точки чтения.
            const uint64_t keepFrom = uint64_t(refIndex) > refKeepFrames_ ? uint64_t(refIndex) - refKeepFrames_ : 0;
            const uint64_t consumed = refRing_.totalRead();
            if (keepFrom > consumed) refRing_.discard(uint32_t(keepFrom - consumed));
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

        chain_->process(micFrame_, &refFrame_);

        outDb_.store(rmsDb(micFrame_.planes()[0], frame), std::memory_order_relaxed);
        if (recording_) {
            micFrame_.toInterleaved(micBuf_.data());
            recOut_.write(micBuf_.data(), frame);
        }

        // Моно -> каналы выхода дублированием.
        const float* mono = micFrame_.planes()[0];
        const uint32_t oc = settings_.outputChannels;
        for (uint32_t i = 0; i < frame; ++i) {
            for (uint32_t c = 0; c < oc; ++c) outBuf_[size_t(i) * oc + c] = mono[i];
        }
        if (outRing_.write(outBuf_.data(), frame) < frame) {
            outOverruns_.fetch_add(1, std::memory_order_relaxed);
        }
        frames_.fetch_add(1, std::memory_order_relaxed);
    }
}

void Engine::fillOutput(float* interleaved, uint32_t frames) {
    const uint32_t n = outRing_.read(interleaved, frames);
    if (n < frames) {
        std::memset(interleaved + size_t(n) * settings_.outputChannels, 0,
                    size_t(frames - n) * settings_.outputChannels * sizeof(float));
        if (frames_.load(std::memory_order_relaxed) > 0) outUnderruns_.fetch_add(1, std::memory_order_relaxed);
    }
}

EngineStatus Engine::status() const {
    EngineStatus s;
    {
        std::lock_guard<std::mutex> g(infoMutex_);
        s = info_;
    }
    s.running = running_.load();
    if (!s.running) return s;
    s.micDb = micDb_.load();
    s.refDb = refDb_.load();
    s.outDb = outDb_.load();
    s.framesProcessed = frames_.load();
    s.refMissing = refMissing_.load();
    s.outUnderruns = outUnderruns_.load();
    s.outOverruns = outOverruns_.load();
    s.micGaps = micAsm_.stats().gaps;
    s.refGaps = refAsm_.stats().gaps;
    s.micDriftPpm = micAsm_.timelineSnapshot().driftPpm();
    s.refDriftPpm = refAsm_.timelineSnapshot().driftPpm();
    s.outBufferedMs = outRing_.readable() * 1000 / fmt_.sampleRate;
    if (chain_) s.stats = chain_->stats();
    for (const std::string& e : {micStream_.lastError(), refStream_.lastError(), output_.lastError(), keepalive_.lastError()}) {
        if (!e.empty()) {
            s.error = e;
            break;
        }
    }
    return s;
}

}  // namespace bomboec
