#include "core/packet_assembler.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace bomboec;
using Catch::Approx;

namespace {
constexpr double kTps = 1e7;  // 100 ns
constexpr double kRate = 48000.0;
constexpr uint32_t kPacket = 480;
constexpr int64_t kPacketTicks = 100'000;  // 10 ms

std::vector<float> ramp(uint32_t frames, float start) {
    std::vector<float> v(frames);
    for (uint32_t i = 0; i < frames; ++i) v[i] = start + float(i);
    return v;
}
}  // namespace

TEST_CASE("PacketAssembler passes contiguous packets through and anchors the timeline") {
    RingBuffer ring(1, 48000);
    PacketAssembler pa;
    pa.configure(kRate, kTps, &ring);

    int64_t t = 5'000'000;
    for (int i = 0; i < 10; ++i) {
        const auto data = ramp(kPacket, float(i * kPacket));
        pa.push(data.data(), kPacket, t + (i % 2 ? 300 : -300));  // джиттер 30 us
        t += kPacketTicks;
    }
    CHECK(ring.readable() == 10 * kPacket);
    CHECK(pa.stats().gaps == 0);
    CHECK(pa.stats().overlaps == 0);
    CHECK(pa.stats().maxJitterMs == Approx(0.06).margin(0.01));
    CHECK(pa.originTicks() == 5'000'000 - 300);  // origin = метка первого пакета как есть
    CHECK(pa.timeline().lastSample() == 9 * kPacket);

    std::vector<float> out(10 * kPacket);
    ring.read(out.data(), uint32_t(out.size()));
    CHECK(out[0] == 0.0f);
    CHECK(out[4800 - 1] == 4799.0f);
}

TEST_CASE("PacketAssembler fills gaps with silence and drops overlaps") {
    RingBuffer ring(2, 48000);
    PacketAssembler pa;
    pa.configure(kRate, kTps, &ring, 2.5);

    std::vector<float> ones(kPacket * 2, 1.0f);
    int64_t t = 0;
    pa.push(ones.data(), kPacket, t);
    // Пропуск 30 ms: loopback молчал.
    t += kPacketTicks + 300'000;
    pa.push(ones.data(), kPacket, t);
    CHECK(pa.stats().gaps == 1);
    CHECK(pa.stats().gapSamples == 1440);
    CHECK(ring.readable() == kPacket + 1440 + kPacket);

    std::vector<float> out((kPacket + 1440 + kPacket) * 2);
    ring.read(out.data(), uint32_t(out.size() / 2));
    CHECK(out[(kPacket - 1) * 2] == 1.0f);
    CHECK(out[kPacket * 2] == 0.0f);
    CHECK(out[(kPacket + 1439) * 2 + 1] == 0.0f);
    CHECK(out[(kPacket + 1440) * 2] == 1.0f);

    // Наложение 5 ms: следующий пакет пришёл раньше ожидаемого на 240 сэмплов.
    t += kPacketTicks - 50'000;
    pa.push(ones.data(), kPacket, t);
    CHECK(pa.stats().overlaps == 1);
    CHECK(pa.stats().overlapSamples == 240);
    CHECK(ring.readable() == kPacket - 240);

    // Джиттер 1 ms ниже порога: ничего не делаем.
    t += kPacketTicks + 10'000;
    pa.push(ones.data(), kPacket, t);
    CHECK(pa.stats().gaps == 1);
    CHECK(pa.stats().overlaps == 1);
    CHECK(pa.stats().maxJitterMs == Approx(1.0).margin(0.01));

    pa.reset();
    CHECK_FALSE(pa.started());
    CHECK(pa.stats().packets == 0);
}
