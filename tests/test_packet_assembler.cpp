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

TEST_CASE("PacketAssembler places a packet with a timestamp error right after the previous one") {
    RingBuffer ring(1, 48000);
    PacketAssembler pa;
    pa.configure(kRate, kTps, &ring);
    std::vector<float> pkt(kPacket, 1.0f);
    int64_t t = 36'000'000'000;  // час аптайма
    for (int i = 0; i < 3; ++i, t += kPacketTicks) pa.push(pkt.data(), kPacket, t);
    pa.push(pkt.data(), kPacket, 0, {.timestampError = true, .discontinuity = true});
    t += kPacketTicks;
    pa.push(pkt.data(), kPacket, t);

    CHECK(pa.stats().timestampErrors == 1);
    CHECK(pa.stats().discontinuities == 1);
    CHECK(pa.stats().gaps == 0);
    CHECK(pa.stats().overlaps == 0);
    CHECK(pa.stats().resyncs == 0);
    CHECK(ring.readable() == 5 * kPacket);
    CHECK(pa.timeline().lastTicks() == t);
    CHECK(pa.timeline().lastSample() == 4 * kPacket);
}

TEST_CASE("PacketAssembler resyncs on a jump beyond the threshold instead of filling silence") {
    RingBuffer ring(1, 48000 * 2);
    PacketAssembler pa;
    pa.configure(kRate, kTps, &ring);
    std::vector<float> pkt(kPacket, 1.0f);
    int64_t t = 10'000'000;
    pa.push(pkt.data(), kPacket, t);
    t += kPacketTicks;
    pa.push(pkt.data(), kPacket, t);

    // Вперёд на 5 с: раньше это 2 с тишины (всё кольцо) и сдвиг таймлайна.
    t += 50'000'000;
    pa.push(pkt.data(), kPacket, t);
    CHECK(pa.stats().resyncs == 1);
    CHECK(pa.stats().gaps == 0);
    CHECK(ring.readable() == 3 * kPacket);
    CHECK(pa.timeline().lastTicks() == t);
    CHECK(pa.timeline().lastSample() == 2 * kPacket);

    // Назад без флага (битая метка): пакет не выбрасывается как наложение.
    pa.push(pkt.data(), kPacket, 0);
    CHECK(pa.stats().resyncs == 2);
    CHECK(pa.stats().overlaps == 0);
    CHECK(ring.readable() == 4 * kPacket);
}

TEST_CASE("PacketAssembler anchors on what the full ring actually took") {
    std::vector<float> pkt(kPacket, 1.0f);
    const int64_t t = 10'000'000;

    SECTION("partial write") {
        RingBuffer ring(1, 1000);
        PacketAssembler pa;
        pa.configure(kRate, kTps, &ring);
        pa.push(pkt.data(), kPacket, t);
        pa.push(pkt.data(), kPacket, t + kPacketTicks);
        pa.push(pkt.data(), kPacket, t + 2 * kPacketTicks);  // влезает 40 из 480
        CHECK(pa.stats().dropped == 440);
        CHECK(pa.timeline().lastTicks() == t + 2 * kPacketTicks);
        CHECK(pa.timeline().lastSample() == 2 * kPacket);  // было 1000 - 480 = 520
    }
    SECTION("nothing written") {
        RingBuffer ring(1, 2 * kPacket);
        PacketAssembler pa;
        pa.configure(kRate, kTps, &ring);
        pa.push(pkt.data(), kPacket, t);
        pa.push(pkt.data(), kPacket, t + kPacketTicks);
        pa.push(pkt.data(), kPacket, t + 2 * kPacketTicks);  // кольцо полно
        CHECK(pa.stats().dropped == kPacket);
        CHECK(pa.timeline().lastTicks() == t + kPacketTicks);  // якорь второго пакета остался
        CHECK(pa.timeline().lastSample() == kPacket);

        std::vector<float> sink(kPacket);
        ring.read(sink.data(), kPacket);
        pa.push(pkt.data(), kPacket, t + 3 * kPacketTicks);
        CHECK(pa.timeline().lastTicks() == t + 3 * kPacketTicks);
        CHECK(pa.timeline().lastSample() == 2 * kPacket);
    }
}

TEST_CASE("PacketAssembler restarts the timeline after the ring dropped samples") {
    std::vector<float> pkt(kPacket, 1.0f);
    RingBuffer ring(1, 2 * kPacket);
    PacketAssembler pa;
    pa.configure(kRate, kTps, &ring);
    const int64_t t = 10'000'000;
    pa.push(pkt.data(), kPacket, t);
    pa.push(pkt.data(), kPacket, t + kPacketTicks);
    pa.push(pkt.data(), kPacket, t + 2 * kPacketTicks);  // кольцо полно: пакет потерян
    CHECK(pa.stats().dropped == kPacket);
    CHECK(pa.stats().resyncs == 0);

    std::vector<float> sink(2 * kPacket);
    ring.read(sink.data(), 2 * kPacket);
    pa.push(pkt.data(), kPacket, t + 3 * kPacketTicks);
    // Между старыми якорями и новым пропал пакет: по ним частота вышла бы 3/4 номинала.
    CHECK(pa.stats().resyncs == 1);
    CHECK(pa.timeline().lastSample() == 2 * kPacket);
    CHECK(pa.timeline().lastTicks() == t + 3 * kPacketTicks);
    pa.push(pkt.data(), kPacket, t + 4 * kPacketTicks);
    CHECK(pa.stats().resyncs == 1);
    CHECK(pa.timeline().estimatedRate() == Approx(kRate));  // окно короче 2 с: номинал, без смещения
}
