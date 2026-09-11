// Выравнивание reference на реалистичной шкале времени: джиттер меток и дрейф часов
// как на машине разработки (README: Yeti до 0.2 ms и +23 ppm к QPC, loopback 0.03 ms,
// колонки около -117 ppm).

#include "sim/pipeline_sim.h"
#include "stages/builtin_stages.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <vector>

using namespace bomboec;
using namespace bomboec::test;

namespace {

void realisticClocks(PipelineSim& sim) {
    sim.mic.jitterMs = 0.2;
    sim.mic.ppm = 23.0;
    sim.ref.jitterMs = 0.03;
    sim.ref.ppm = -117.0;
}

struct Continuity {
    size_t frames = 0;
    size_t jumps = 0;  // кадров, где reference не продолжил предыдущий кадр
    int64_t maxJump = 0;
    double errorSpread = 0.0;  // размах ошибки выравнивания, сэмплы
};

Continuity continuity(const PipelineSim& sim, size_t skipFrames) {
    Continuity c;
    const auto& rec = sim.probe->records;
    const double lead = sim.settings.referenceLeadMs / 1000.0;
    double lo = 1e9, hi = -1e9;
    for (size_t k = skipFrames; k < rec.size(); ++k) {
        if (rec[k].refFirst == 0.0f || rec[k - 1].refLast == 0.0f) continue;
        ++c.frames;
        const auto jump = int64_t(rec[k].refFirst) - int64_t(rec[k - 1].refLast) - 1;
        if (jump != 0) ++c.jumps;
        c.maxJump = std::max<int64_t>(c.maxJump, std::llabs(jump));
        const double expected = sim.ref.indexAt(sim.mic.timeOf(double(rec[k].mic) - 1.0) - lead);
        const double err = double(rec[k].refFirst) - 1.0 - expected;
        lo = std::min(lo, err);
        hi = std::max(hi, err);
    }
    c.errorSpread = hi - lo;
    return c;
}

}  // namespace

TEST_CASE("Reference stays continuous under timestamp jitter and clock drift", "[pipeline][alignment]") {
    PipelineSim sim;
    realisticClocks(sim);
    REQUIRE(sim.start());
    sim.run(60.0);

    const Continuity c = continuity(sim, 300);
    INFO("frames " << c.frames << ", jumps " << c.jumps << ", max |jump| " << c.maxJump << ", error spread "
                   << c.errorSpread << " samples");
    REQUIRE(c.frames > 5000);
    // Относительный дрейф 140 ppm = 0.067 сэмпла на кадр: проскальзывание на один сэмпл
    // примерно раз в 15 кадров, остальные кадры непрерывны.
    CHECK(c.jumps < c.frames / 10);
    CHECK(c.maxJump <= 1);
    CHECK(c.errorSpread <= 6.0);
    CHECK(sim.pipeline.stats().refMissing <= 3);
}

namespace {

struct AecResult {
    double attenuationDb = 0.0;  // за последние 10 с
    double erleDb = 0.0;
    uint64_t refJumps = 0;
};

// Reference: белый шум (стерео, второй канал 0.8 первого). Микрофон слышит только эхо:
// моно-сумму reference с задержкой 30 ms и ослаблением 0.5 по истинному времени.
AecResult cancelEcho(PipelineSim& sim, double seconds, bool speechBand = false) {
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.1f);
    std::vector<float> refMono(size_t((seconds + 2.0) * 48000.0));
    for (float& x : refMono) x = noise(rng);
    if (speechBand) {
        // Два биквада нижних частот 4 kHz (RBJ, Q 0.707): полоса, в которой работает линейный фильтр AEC3.
        const double w0 = 2.0 * 3.14159265358979 * 4000.0 / 48000.0, alpha = std::sin(w0) / (2.0 * 0.7071);
        const double a0 = 1.0 + alpha, b0 = (1.0 - std::cos(w0)) / 2.0 / a0, b1 = (1.0 - std::cos(w0)) / a0;
        const double a1 = -2.0 * std::cos(w0) / a0, a2 = (1.0 - alpha) / a0;
        for (int pass = 0; pass < 2; ++pass) {
            double z1 = 0.0, z2 = 0.0;
            for (float& x : refMono) {
                const double y = b0 * x + z1;
                z1 = b1 * x - a1 * y + z2;
                z2 = b0 * x - a2 * y;
                x = float(y * 2.0);
            }
        }
    }
    const auto refAt = [&](double index) {
        if (index < 0.0 || index + 1.0 >= double(refMono.size())) return 0.0f;
        const auto i = size_t(index);
        const auto f = float(index - double(i));
        return refMono[i] + (refMono[i + 1] - refMono[i]) * f;
    };
    sim.refSignal = [&](uint64_t i, uint32_t ch) {
        return i < refMono.size() ? refMono[i] * (ch == 0 ? 1.0f : 0.8f) : 0.0f;
    };
    std::vector<float> micIn;
    micIn.reserve(size_t(seconds * 48000.0) + 48000);
    sim.micSignal = [&](uint64_t i, uint32_t) {
        const double t = sim.mic.timeOf(double(i)) - 0.030;
        const float echo = 0.5f * 0.9f * refAt(sim.ref.indexAt(t));
        if (i >= micIn.size()) micIn.push_back(echo);
        return echo;
    };

    auto chain = std::make_unique<Chain>();
    toml::table params;
    params.insert("aec", true);
    chain->add(makeWebrtcStage(), params);
    REQUIRE(sim.start(std::move(chain)));
    sim.run(seconds);

    // Последние 10 с: энергия эха на входе и остатка на выходе.
    const size_t from = micIn.size() - 480000, to = micIn.size();
    double in = 0.0, out = 0.0;
    for (size_t i = from; i < to; ++i) in += double(micIn[i]) * micIn[i];
    const size_t outTo = std::min(sim.output.size(), to);
    for (size_t i = outTo - 480000; i < outTo; ++i) out += double(sim.output[i]) * sim.output[i];
    AecResult r;
    r.attenuationDb = 10.0 * std::log10(in / std::max(out, 1e-12));
    r.erleDb = sim.pipeline.chainStats().erleDb.value_or(-1.0);
    r.refJumps = sim.pipeline.stats().refJumps;
    sim.micSignal = nullptr;  // лямбды ссылаются на локальные буферы
    sim.refSignal = nullptr;
    return r;
}

}  // namespace

// Сколько подавления стоит каждое искажение шкалы времени. Скрытый (запуск:
// bomboec_tests "[aec-benchmark]"). На старом выравнивании (чтение прямо по
// предсказанию таймлайнов) джиттер + дрейф давали 0.6 dB на белом шуме; с
// непрерывной позицией (сентябрь 2026):
//   белый шум:   идеально 13.0, джиттер 13.0, дрейф 11.6, оба 11.6 dB
//   полоса речи: 17.6 dB во всех четырёх вариантах
// Джиттер больше не стоит ничего; остаток на белом шуме от проскальзываний на
// сэмпл при дрейфе (их убрал бы адаптивный ресемплинг reference).
TEST_CASE("AEC3 attenuation by timing impairment", "[.aec-benchmark]") {
    for (int band = 0; band < 2; ++band) {
        for (int variant = 0; variant < 4; ++variant) {
            PipelineSim sim;
            if (variant & 1) {
                sim.mic.jitterMs = 0.2;
                sim.ref.jitterMs = 0.03;
            }
            if (variant & 2) {
                sim.mic.ppm = 23.0;
                sim.ref.ppm = -117.0;
            }
            const AecResult r = cancelEcho(sim, 40.0, band == 1);
            WARN("speechBand " << band << " jitter " << (variant & 1) << " drift " << (variant >> 1) << ": "
                               << r.attenuationDb << " dB, jumps " << r.refJumps);
        }
    }
}

// Пороги ниже измеренного (см. комментарий к бенчмарку) примерно на 1.5 dB. Белый шум
// на всю полосу 48 kHz: линейный фильтр AEC3 работает только в нижней полосе, поэтому
// полное подавление здесь около 13 dB и в идеальном случае.
TEST_CASE("AEC3 cancels echo through the pipeline with ideal clocks", "[pipeline][aec]") {
    // Контроль стенда: без джиттера и дрейфа reference непрерывен при любом выравнивании.
    PipelineSim sim;
    const AecResult r = cancelEcho(sim, 40.0);
    INFO("attenuation " << r.attenuationDb << " dB, ref jumps " << r.refJumps);
    CHECK(r.attenuationDb > 11.0);
}

TEST_CASE("AEC3 cancels echo on a jittered, drifting timeline", "[pipeline][aec]") {
    PipelineSim sim;
    realisticClocks(sim);
    SECTION("white noise") {
        const AecResult r = cancelEcho(sim, 40.0);
        INFO("attenuation " << r.attenuationDb << " dB, ref jumps " << r.refJumps);
        CHECK(r.attenuationDb > 10.0);  // было 0.6 dB при чтении прямо по предсказанию
    }
    SECTION("speech band") {
        const AecResult r = cancelEcho(sim, 40.0, true);
        INFO("attenuation " << r.attenuationDb << " dB, ref jumps " << r.refJumps);
        CHECK(r.attenuationDb > 15.0);
    }
}
