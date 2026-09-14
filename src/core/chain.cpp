#include "core/chain.h"

namespace bomboec {

void Chain::add(std::unique_ptr<IStage> stage, toml::table params) {
    entries_.push_back({std::move(stage), std::move(params)});
}

bool Chain::init(const PipelineFormat& fmt, std::string& error) {
    caps_ = 0;
    latencyFrames_ = 0;
    warnings_.clear();
    for (size_t i = 0; i < entries_.size(); ++i) {
        IStage& s = *entries_[i].stage;
        StageParams params(entries_[i].params);
        if (!s.init(fmt, params, error)) {
            error = "stage[" + std::to_string(i) + "] init failed: " + error;
            return false;
        }
        const StageInfo info = s.info();
        for (const std::string& key : params.unknownKeys()) {
            warnings_.push_back("config: chain[" + std::to_string(i) + "] (" + info.id + "): unknown key '" + key +
                                "' ignored");
        }
        if (info.sampleRate != fmt.sampleRate || info.frameSamples != fmt.frameSamples) {
            error = "stage '" + info.id + "' format mismatch: wants " + std::to_string(info.sampleRate) + " Hz / " +
                    std::to_string(info.frameSamples) + " samples, pipeline is " + std::to_string(fmt.sampleRate) +
                    " Hz / " + std::to_string(fmt.frameSamples);
            return false;
        }
        for (const Cap c : {Cap::Hpf, Cap::Aec, Cap::Ns, Cap::Agc, Cap::Limiter, Cap::Transient}) {
            if (hasCap(info.caps, c) && hasCap(caps_, c)) {
                error = "capability '" + std::string(capName(c)) + "' declared twice (stage '" + info.id + "')";
                return false;
            }
        }
        caps_ |= info.caps;
        latencyFrames_ += info.latencyFrames;
    }
    return true;
}

void Chain::process(Frame& mic, const Frame* reference) {
    for (const Entry& e : entries_) {
        e.stage->process(mic, reference);
    }
}

void Chain::reset() {
    for (const Entry& e : entries_) e.stage->reset();
}

StageStats Chain::stats() const {
    StageStats out;
    for (const Entry& e : entries_) {
        const StageStats s = e.stage->stats();
        if (!out.delayMs && s.delayMs) out.delayMs = s.delayMs;
        if (!out.erlDb && s.erlDb) out.erlDb = s.erlDb;
        if (!out.erleDb && s.erleDb) out.erleDb = s.erleDb;
        if (!out.residualEchoLikelihood && s.residualEchoLikelihood) {
            out.residualEchoLikelihood = s.residualEchoLikelihood;
        }
        if (!out.gainDb && s.gainDb) out.gainDb = s.gainDb;
        if (!out.vadProbability && s.vadProbability) out.vadProbability = s.vadProbability;
        out.errors += s.errors;
        out.transients += s.transients;
    }
    return out;
}

std::unique_ptr<Chain> buildChain(const StageRegistry& registry, const AppConfig& cfg, std::string& error) {
    auto chain = std::make_unique<Chain>();
    for (const StageConfig& sc : cfg.chain) {
        std::unique_ptr<IStage> stage = registry.create(sc.id);
        if (!stage) {
            error = "unknown stage id '" + sc.id + "'";
            return nullptr;
        }
        chain->add(std::move(stage), sc.params);
    }
    return chain;
}

}  // namespace bomboec
