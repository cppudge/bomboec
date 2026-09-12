#pragma once

// Симулятор устройств для тестов Pipeline: три независимых часа (микрофон,
// loopback, выход), пакеты по 10 ms с QPC-метками и джиттером, события в порядке
// времени в одном потоке. Сигналы по умолчанию кодируют индекс сэмпла в значении
// (mic = индекс mic-сэмпла + 1, ref = индекс ref-сэмпла + 1), поэтому ProbeStage
// видит, какой кусок reference движок сопоставил каждому кадру микрофона.

#include "core/chain.h"
#include "engine/pipeline.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace bomboec::test {

// Часы устройства: сэмпл n звучит в момент t0 + n / (rate * (1 + ppm)).
struct SimClock {
    double t0 = 1.0;  // с
    double ppm = 0.0;
    double jitterMs = 0.0;  // равномерный шум метки пакета, ±
    double rate = 48000.0;

    double timeOf(double sampleIndex) const { return t0 + sampleIndex / (rate * (1.0 + ppm * 1e-6)); }
    double indexAt(double t) const { return (t - t0) * rate * (1.0 + ppm * 1e-6); }
};

// Стадия-зонд: запоминает первый сэмпл mic и первый/последний сэмпл reference каждого кадра.
class ProbeStage final : public IStage {
public:
    struct Record {
        float mic;
        float refFirst;
        float refLast;
    };

    StageInfo info() const override { return {"probe", fmt_.sampleRate, fmt_.frameSamples, capBit(Cap::Aec), 0}; }
    bool init(const PipelineFormat& fmt, StageParams&, std::string&) override {
        fmt_ = fmt;
        records.reserve(1 << 16);
        return true;
    }
    void process(Frame& mic, const Frame* reference) override {
        const auto r = reference->channel(0);
        records.push_back({mic.channel(0)[0], r[0], r[r.size() - 1]});
    }
    void reset() override {}
    StageStats stats() const override { return {}; }

    std::vector<Record> records;

private:
    PipelineFormat fmt_;
};

struct PipelineSim {
    PipelineFormat fmt;
    EngineSettings settings;
    SimClock mic, ref, out;
    // Пакет доступен через столько после своего последнего сэмпла. Loopback отдаёт данные
    // раньше их метки: audio engine смешивает на период вперёд, а метка пакета - время, когда
    // сэмплы прозвучат (на машине разработки refMissing = 0 даже при lead 0). Микрофон
    // отдаёт пакет после его захвата плюс доставка (USB, период engine).
    double micDeliveryMs = 2.0;
    double refDeliveryMs = -8.0;
    // Пакеты микрофона, которые должны были прийти в [stallFrom, stallTo), приходят разом в stallTo.
    double stallFrom = -1.0, stallTo = -1.0;
    // Блок, которым render-поток читает выход (0 = кадр). У WASAPI выхода он больше запаса
    // outputBufferMs: обрезка излишка не должна опустошать кольцо ниже одного блока.
    uint32_t outBlockFrames = 0;
    // Пакеты reference в [refLostFrom, refLostTo) не приходят вовсе (loopback остановился:
    // устройство пропало, потом вернулось).
    double refLostFrom = -1.0, refLostTo = -1.0;
    // Правка метки/флагов пакета микрофона по его номеру (битые метки и т.п.).
    std::function<void(uint64_t packet, int64_t& ticks, PacketFlags& flags)> tweakMic;
    std::function<float(uint64_t index, uint32_t channel)> micSignal = [](uint64_t i, uint32_t) {
        return float(i + 1);
    };
    std::function<float(uint64_t index, uint32_t channel)> refSignal = [](uint64_t i, uint32_t) {
        return float(i + 1);
    };

    Pipeline pipeline;
    ProbeStage* probe = nullptr;
    std::vector<float> output;        // канал 0 всего, что отдал fillOutput
    std::vector<double> outputTimes;  // момент чтения каждого блока fillOutput (по одному на блок)
    double now = 1.0;                 // как SimClock::t0 по умолчанию: run(10) = 10 с звука

    // Цепочка: chain (или одна ProbeStage, если nullptr).
    bool start(std::unique_ptr<Chain> chain = nullptr) {
        if (!chain) {
            chain = std::make_unique<Chain>();
            auto p = std::make_unique<ProbeStage>();
            probe = p.get();
            chain->add(std::move(p));
        }
        std::string error;
        if (!chain->init(fmt, error)) return false;
        return pipeline.configure(fmt, settings, std::move(chain), error);
    }

    void run(double seconds) {
        const double end = now + seconds;
        const uint32_t packet = fmt.frameSamples;
        for (;;) {
            const double tRef = ref.timeOf(double(refPos_ + packet)) + refDeliveryMs / 1000.0;
            const double tMic = micDelivery(mic.timeOf(double(micPos_ + packet)) + micDeliveryMs / 1000.0);
            const double tOut = out.timeOf(double(outPos_ + outBlock()));
            const double t = std::min({tRef, tMic, tOut});
            if (t > end) break;
            now = t;
            if (t == tRef) pushRef();
            else if (t == tMic) pushMic();
            else pullOut();
        }
        now = end;
    }

private:
    double micDelivery(double t) const { return (t >= stallFrom && t < stallTo) ? stallTo : t; }

    int64_t stamp(const SimClock& c, uint64_t index) {
        const double noise = jitter_(rng_) * c.jitterMs / 1000.0;
        return std::llround((c.timeOf(double(index)) + noise) * 1e7);
    }

    void pushRef() {
        const uint32_t n = fmt.frameSamples, ch = fmt.referenceChannels;
        const double t = ref.timeOf(double(refPos_));
        if (t >= refLostFrom && t < refLostTo) {
            refPos_ += n;  // пакет потерян: loopback его не отдал
            return;
        }
        buf_.resize(size_t(n) * ch);
        for (uint32_t i = 0; i < n; ++i) {
            for (uint32_t c = 0; c < ch; ++c) buf_[size_t(i) * ch + c] = refSignal(refPos_ + i, c);
        }
        pipeline.onRefPacket(buf_.data(), n, stamp(ref, refPos_));
        refPos_ += n;
    }

    void pushMic() {
        const uint32_t n = fmt.frameSamples, ch = fmt.micChannels;
        buf_.resize(size_t(n) * ch);
        for (uint32_t i = 0; i < n; ++i) {
            for (uint32_t c = 0; c < ch; ++c) buf_[size_t(i) * ch + c] = micSignal(micPos_ + i, c);
        }
        int64_t ticks = stamp(mic, micPos_);
        PacketFlags flags;
        if (tweakMic) tweakMic(micPos_ / n, ticks, flags);
        pipeline.onMicPacket(buf_.data(), n, ticks, flags);
        micPos_ += n;
    }

    uint32_t outBlock() const { return outBlockFrames ? outBlockFrames : fmt.frameSamples; }

    void pullOut() {
        const uint32_t n = outBlock(), ch = settings.outputChannels;
        outBuf_.resize(size_t(n) * ch);
        pipeline.fillOutput(outBuf_.data(), n);
        for (uint32_t i = 0; i < n; ++i) output.push_back(outBuf_[size_t(i) * ch]);
        outputTimes.push_back(now);
        outPos_ += n;
    }

    uint64_t micPos_ = 0, refPos_ = 0, outPos_ = 0;
    std::mt19937 rng_{1};
    std::uniform_real_distribution<double> jitter_{-1.0, 1.0};
    std::vector<float> buf_, outBuf_;
};

}  // namespace bomboec::test
