#include "engine/engine.h"

#include "stages/builtin_stages.h"
#include "wasapi/devices.h"

#include <algorithm>
#include <memory>

namespace bomboec {

Engine::Engine() = default;
Engine::~Engine() { stop(); }

bool Engine::start(const AppConfig& cfg, std::string& error) {
    stop();
    if (open(cfg, error)) return true;
    // Всё, что успело открыться (клиенты WASAPI, файлы debug-записи), закрывается
    // сразу, а не висит до следующего старта или выхода.
    stop();
    return false;
}

bool Engine::open(const AppConfig& cfg, std::string& error) {
    const PipelineFormat& fmt = cfg.format;
    const EngineSettings& settings = cfg.engine;

    StageRegistry registry;
    registerBuiltinStages(registry);
    std::unique_ptr<Chain> chain = buildChain(registry, cfg, error);
    if (!chain || !chain->init(fmt, error)) return false;

    namespace ws = wasapi;
    const ws::ComPtr<IMMDevice> micDev = ws::openDevice(ws::Flow::Capture, ws::fromUtf8(settings.micId), error);
    if (!micDev) return false;
    const ws::ComPtr<IMMDevice> spkDev = ws::openDevice(ws::Flow::Render, ws::fromUtf8(settings.speakersId), error);
    if (!spkDev) return false;
    const ws::ComPtr<IMMDevice> outDev = ws::openDevice(ws::Flow::Render, ws::fromUtf8(settings.outputId), error);
    if (!outDev) return false;
    const ws::DeviceInfo micInfo = ws::describeDevice(micDev.Get());
    const ws::DeviceInfo spkInfo = ws::describeDevice(spkDev.Get());
    const ws::DeviceInfo outInfo = ws::describeDevice(outDev.Get());
    if (spkInfo.id == outInfo.id) {
        // Очищенный микрофон в те же колонки = акустическая петля.
        error = "output device must differ from the reference speakers (" + ws::toUtf8(outInfo.name) + ")";
        return false;
    }
    if (ws::sameVirtualDevice(micInfo, outInfo)) {
        // Микрофон кабеля при выходе в тот же кабель: движок слушал бы сам себя и молча выдавал
        // тишину. Типично сразу после установки драйвера: Windows делает кабель устройством
        // по умолчанию и для ввода.
        error = "microphone '" + ws::toUtf8(micInfo.name) + "' is the other end of the output '" +
                ws::toUtf8(outInfo.name) + "' (digital loop): choose the physical microphone";
        return false;
    }
    {
        const std::scoped_lock g(infoMutex_);
        info_ = {};
        info_.micName = ws::toUtf8(micInfo.name);
        info_.speakersName = ws::toUtf8(spkInfo.name);
        info_.outputName = ws::toUtf8(outInfo.name);
        info_.referenceLeadMs = settings.referenceLeadMs;
    }

    if (!pipeline_.configure(fmt, settings, std::move(chain), error)) return false;

    const auto flags = [](const ws::CapturePacket& p) { return PacketFlags{p.timestampError, p.discontinuity}; };
    const uint32_t rate = fmt.sampleRate;
    ws::CaptureStream::Options micOpt;
    micOpt.channels = fmt.micChannels;
    micOpt.raw = settings.micRaw;
    micOpt.sampleRate = rate;
    const auto onMic = [this, flags](const ws::CapturePacket& p) {
        pipeline_.onMicPacket(p.interleaved, p.frames, p.qpc100ns, flags(p));
    };
    if (!micStream_.open(micDev.Get(), micOpt, onMic, error)) return false;

    ws::CaptureStream::Options refOpt;
    refOpt.channels = fmt.referenceChannels;
    refOpt.loopback = true;
    refOpt.sampleRate = rate;
    const auto onRef = [this, flags](const ws::CapturePacket& p) {
        pipeline_.onRefPacket(p.interleaved, p.frames, p.qpc100ns, flags(p));
    };
    if (!refStream_.open(spkDev.Get(), refOpt, onRef, error)) return false;

    ws::RenderStream::Options keepOpt;
    keepOpt.sampleRate = rate;
    if (!keepalive_.open(spkDev.Get(), keepOpt, nullptr, error)) return false;

    ws::RenderStream::Options outOpt;
    outOpt.sampleRate = rate;
    outOpt.channels = settings.outputChannels;
    // Ёмкость буфера WASAPI не задержка: заполняется он только до targetMs.
    outOpt.bufferMs = std::max<uint32_t>(40, settings.outputRenderMs + 20);
    outOpt.targetMs = settings.outputRenderMs;
    if (!output_.open(outDev.Get(), outOpt, [this](float* buf, uint32_t n) { pipeline_.fillOutput(buf, n); }, error)) {
        return false;
    }
    {
        const std::scoped_lock g(infoMutex_);
        info_.micRaw = micStream_.rawApplied();
        info_.micEventDriven = micStream_.eventDriven();
        info_.refEventDriven = refStream_.eventDriven();
        info_.micDeviceChannels = micStream_.deviceChannels();
        info_.refDeviceChannels = refStream_.deviceChannels();
        info_.outRenderMs = output_.targetFrames() * 1000 / rate;
    }

    running_.store(true);
    return keepalive_.start(error) && refStream_.start(error) && micStream_.start(error) && output_.start(error);
}

void Engine::stop() {
    running_.store(false);
    micStream_.close();
    refStream_.close();
    output_.close();
    keepalive_.close();
    pipeline_.reset();  // потоки собраны: запись и цепочку можно закрывать
}

EngineStatus Engine::status() const {
    EngineStatus s;
    {
        const std::scoped_lock g(infoMutex_);
        s = info_;
    }
    s.running = running_.load();
    if (!s.running) return s;
    static_cast<PipelineStats&>(s) = pipeline_.stats();
    s.stats = pipeline_.chainStats();
    s.mmcss =
        micStream_.mmcssApplied() && refStream_.mmcssApplied() && output_.mmcssApplied() && keepalive_.mmcssApplied();
    for (const std::string& e :
         {micStream_.lastError(), refStream_.lastError(), output_.lastError(), keepalive_.lastError()}) {
        if (!e.empty()) {
            s.error = e;
            break;
        }
    }
    return s;
}

}  // namespace bomboec
