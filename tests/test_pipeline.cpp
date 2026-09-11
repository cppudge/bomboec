#include "sim/pipeline_sim.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace bomboec;
using namespace bomboec::test;

namespace {

// Ошибка выравнивания каждого кадра в сэмплах reference: какой индекс выбран минус
// какой должен быть по истинным часам (время первого сэмпла кадра mic минус lead).
std::vector<double> alignmentErrors(const PipelineSim& sim, size_t skipFrames) {
    std::vector<double> errors;
    const double lead = sim.settings.referenceLeadMs / 1000.0;
    const auto& records = sim.probe->records;
    for (size_t k = skipFrames; k < records.size(); ++k) {
        if (records[k].refFirst == 0.0f) continue;  // reference не было
        const double micIndex = double(records[k].mic) - 1.0;
        const double expected = sim.ref.indexAt(sim.mic.timeOf(micIndex) - lead);
        errors.push_back(double(records[k].refFirst) - 1.0 - expected);
    }
    return errors;
}

double maxAbs(const std::vector<double>& v) {
    double m = 0.0;
    for (const double x : v) m = std::max(m, std::abs(x));
    return m;
}

// Задержка микрофон -> выход для каждого блока fillOutput после fromSeconds: момент
// чтения минус истинное время mic-сэмпла, который в блоке первый.
std::vector<double> outputLatencyMs(const PipelineSim& sim, double fromSeconds) {
    std::vector<double> latency;
    const size_t block = sim.fmt.frameSamples;
    for (size_t b = 0; b < sim.outputTimes.size(); ++b) {
        const float v = sim.output[b * block];
        if (sim.outputTimes[b] < sim.mic.t0 + fromSeconds || v < 1.0f) continue;
        latency.push_back((sim.outputTimes[b] - sim.mic.timeOf(double(v) - 1.0)) * 1000.0);
    }
    return latency;
}

}  // namespace

TEST_CASE("Pipeline aligns the reference by the timeline and passes the microphone through", "[pipeline]") {
    PipelineSim sim;
    REQUIRE(sim.start());
    sim.run(10.0);

    const PipelineStats s = sim.pipeline.stats();
    CHECK(s.framesProcessed >= 990);
    CHECK(s.refMissing <= 2);
    CHECK(s.refJumps <= 2);  // первые кадры: время t - lead раньше начала reference
    CHECK(s.outUnderruns == 0);
    CHECK(s.outOverruns == 0);

    const std::vector<double> errors = alignmentErrors(sim, 10);
    REQUIRE(errors.size() > 900);
    CHECK(maxAbs(errors) <= 1.0);

    // Кадр 10 ms + доставка пакета 1 ms + фаза чтения + запас output_buffer_ms (10 ms).
    const std::vector<double> latency = outputLatencyMs(sim, 2.0);
    REQUIRE(!latency.empty());
    const auto [lo, hi] = std::minmax_element(latency.begin(), latency.end());
    CHECK(*lo >= 20.0);
    CHECK(*hi <= 40.0);
    CHECK(*hi - *lo < 0.5);
}

TEST_CASE("Pipeline recovers from a bogus microphone timestamp without a latency jump", "[pipeline]") {
    PipelineSim sim;
    sim.tweakMic = [](uint64_t packet, int64_t& ticks, PacketFlags&) {
        if (packet == 500) ticks = 0;  // метка без флага timestampError
    };
    REQUIRE(sim.start());
    sim.run(10.0);

    const PipelineStats s = sim.pipeline.stats();
    CHECK(s.micResyncs == 2);  // на битом пакете и на следующем
    CHECK(s.outUnderruns == 0);
    CHECK(s.outOverruns == 0);
    CHECK(s.refMissing <= 3);
    CHECK(s.outBufferedMs <= 30);
    CHECK(maxAbs(alignmentErrors(sim, 510)) <= 1.0);
    const std::vector<double> latency = outputLatencyMs(sim, 6.0);
    REQUIRE(!latency.empty());
    CHECK(*std::max_element(latency.begin(), latency.end()) <= 40.0);
}
