#include "engine/engine.h"

#include "stages/builtin_stages.h"

#include <algorithm>
#include <memory>

namespace bomboec {

namespace ws = wasapi;

namespace {

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const std::string& p : parts) {
        if (p.empty()) continue;
        if (!out.empty()) out += "; ";
        out += p;
    }
    return out;
}

}  // namespace

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
    cfg_ = cfg;
    const PipelineFormat& fmt = cfg.format;
    const EngineSettings& settings = cfg.engine;

    StageRegistry registry;
    registerBuiltinStages(registry);
    std::unique_ptr<Chain> chain = buildChain(registry, cfg, error);
    if (!chain || !chain->init(fmt, error)) return false;
    std::vector<std::string> warnings = chain->warnings();
    if (!hasCap(chain->caps(), Cap::Aec)) {
        warnings.emplace_back("no stage in [[chain]] cancels echo (aec): the microphone goes out unprocessed");
    }
    if (fmt.micChannels > 1) {
        warnings.push_back("mic_channels = " + std::to_string(fmt.micChannels) +
                           ": only channel 0 of the microphone goes to the output");
    }

    const ws::ComPtr<IMMDevice> micDev = ws::openDevice(ws::Flow::Capture, ws::fromUtf8(settings.micId), error);
    if (!micDev) return false;
    const ws::ComPtr<IMMDevice> outDev = ws::openDevice(ws::Flow::Render, ws::fromUtf8(settings.outputId), error);
    if (!outDev) return false;
    micInfo_ = ws::describeDevice(micDev.Get());
    outInfo_ = ws::describeDevice(outDev.Get());
    if (ws::sameVirtualDevice(micInfo_, outInfo_)) {
        // Микрофон кабеля при выходе в тот же кабель: движок слушал бы сам себя и молча выдавал
        // тишину. Типично сразу после установки драйвера: Windows делает кабель устройством
        // по умолчанию и для ввода.
        error = "microphone '" + ws::toUtf8(micInfo_.name) + "' is the other end of the output '" +
                ws::toUtf8(outInfo_.name) + "' (digital loop): choose the physical microphone";
        return false;
    }
    {
        const std::scoped_lock g(infoMutex_);
        info_ = {};
        info_.micName = ws::toUtf8(micInfo_.name);
        info_.micId = ws::toUtf8(micInfo_.id);
        info_.outputName = ws::toUtf8(outInfo_.name);
        info_.outputId = ws::toUtf8(outInfo_.id);
        info_.referenceLeadMs = settings.referenceLeadMs;
        configWarning_ = join(warnings);
        refWarning_.clear();
    }

    if (!pipeline_.configure(fmt, settings, std::move(chain), error)) return false;

    const uint32_t rate = fmt.sampleRate;
    ws::CaptureStream::Options micOpt;
    micOpt.channels = fmt.micChannels;
    micOpt.raw = settings.micRaw;
    micOpt.sampleRate = rate;
    const auto onMic = [this](const ws::CapturePacket& p) {
        pipeline_.onMicPacket(p.interleaved, p.frames, p.qpc100ns, PacketFlags{p.timestampError, p.discontinuity});
    };
    if (!micStream_.open(micDev.Get(), micOpt, onMic, error)) return false;

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
        info_.micDeviceChannels = micStream_.deviceChannels();
        info_.outRenderMs = output_.targetFrames() * 1000 / rate;
    }

    std::string refWarning;
    if (!openReference(refWarning)) setReferenceWarning(refWarning);

    running_.store(true);
    return micStream_.start(error) && output_.start(error);
}

bool Engine::openReference(std::string& warning) {
    closeReference();
    const EngineSettings& settings = cfg_.engine;
    std::string error;
    const ws::ComPtr<IMMDevice> spkDev = ws::openDevice(ws::Flow::Render, ws::fromUtf8(settings.speakersId), error);
    if (!spkDev) {
        warning = "reference speakers unavailable, echo is not cancelled: " + error;
        return false;
    }
    const ws::DeviceInfo spkInfo = ws::describeDevice(spkDev.Get());
    if (spkInfo.id == outInfo_.id) {
        // Очищенный микрофон в те же колонки = акустическая петля.
        warning = "reference speakers are the output device (" + ws::toUtf8(outInfo_.name) +
                  "): choose the physical speakers, echo is not cancelled";
        return false;
    }
    if (ws::sameVirtualDevice(micInfo_, spkInfo)) {
        // Микрофон кабеля с его же Speakers как reference: AEC вычитал бы сам сигнал.
        warning = "reference '" + ws::toUtf8(spkInfo.name) + "' is the other end of the microphone '" +
                  ws::toUtf8(micInfo_.name) + "': choose the physical speakers, echo is not cancelled";
        return false;
    }

    const uint32_t rate = cfg_.format.sampleRate;
    ws::CaptureStream::Options refOpt;
    refOpt.channels = cfg_.format.referenceChannels;
    refOpt.loopback = true;
    refOpt.sampleRate = rate;
    const auto onRef = [this](const ws::CapturePacket& p) {
        pipeline_.onRefPacket(p.interleaved, p.frames, p.qpc100ns, PacketFlags{p.timestampError, p.discontinuity});
    };
    ws::RenderStream::Options keepOpt;
    keepOpt.sampleRate = rate;
    if (!refStream_.open(spkDev.Get(), refOpt, onRef, error) ||
        !keepalive_.open(spkDev.Get(), keepOpt, nullptr, error) || !keepalive_.start(error) ||
        !refStream_.start(error)) {
        closeReference();
        warning = "reference (loopback of '" + ws::toUtf8(spkInfo.name) + "') failed, echo is not cancelled: " + error;
        return false;
    }
    {
        const std::scoped_lock g(infoMutex_);
        info_.speakersName = ws::toUtf8(spkInfo.name);
        info_.speakersId = ws::toUtf8(spkInfo.id);
        info_.refEventDriven = refStream_.eventDriven();
        info_.refDeviceChannels = refStream_.deviceChannels();
        refWarning_.clear();
    }
    refActive_.store(true);
    return true;
}

void Engine::closeReference() {
    refActive_.store(false);
    refStream_.close();
    keepalive_.close();
}

void Engine::setReferenceWarning(const std::string& text) {
    const std::scoped_lock g(infoMutex_);
    refWarning_ = text;
}

bool Engine::reopenReference(std::string& error) {
    if (!running_.load()) {
        error = "engine is not running";
        return false;
    }
    std::string warning;
    if (openReference(warning)) return true;
    setReferenceWarning(warning);
    error = warning;
    return false;
}

void Engine::stop() {
    running_.store(false);
    micStream_.close();
    closeReference();
    output_.close();
    pipeline_.reset();  // потоки собраны: запись и цепочку можно закрывать
}

EngineStatus Engine::status() const {
    EngineStatus s;
    std::string refWarning, configWarning;
    {
        const std::scoped_lock g(infoMutex_);
        s = info_;
        refWarning = refWarning_;
        configWarning = configWarning_;
    }
    s.running = running_.load();
    if (!s.running) return s;
    static_cast<PipelineStats&>(s) = pipeline_.stats();
    s.stats = pipeline_.chainStats();
    s.referenceActive = refActive_.load();
    s.mmcss = micStream_.mmcssApplied() && output_.mmcssApplied() &&
              (!s.referenceActive || (refStream_.mmcssApplied() && keepalive_.mmcssApplied()));
    for (const std::string& e : {micStream_.lastError(), output_.lastError()}) {
        if (!e.empty()) {
            s.error = e;
            break;
        }
    }
    // Ошибка loopback или keepalive не фатальна: колонки пропали, микрофон работает дальше.
    if (s.referenceActive) {
        for (const std::string& e : {refStream_.lastError(), keepalive_.lastError()}) {
            if (!e.empty()) {
                s.referenceActive = false;
                refWarning = "reference stream stopped, echo is not cancelled: " + e;
                break;
            }
        }
    }
    s.warning = join({configWarning, refWarning});
    return s;
}

}  // namespace bomboec
