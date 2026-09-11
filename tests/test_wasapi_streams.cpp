#include "fakes/fake_wasapi.h"
#include "wasapi/capture_stream.h"
#include "wasapi/render_stream.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

using namespace bomboec;
using namespace bomboec::test;
using bomboec::wasapi::CapturePacket;
using bomboec::wasapi::CaptureStream;
using bomboec::wasapi::RenderStream;

namespace {

bool waitFor(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

TEST_CASE("CaptureStream delivers packets and downmixes when the engine refuses mono", "[wasapi]") {
    FakeAudio audio;
    audio.acceptChannels = 2;  // только стерео: в моно CaptureStream сводит сам
    audio.pendingPackets = 3;
    const auto dev = makeFakeDevice(audio);

    std::atomic<uint32_t> frames{0};
    std::atomic<float> sample{0.0f};
    CaptureStream s;
    CaptureStream::Options o;
    o.channels = 1;
    std::string error;
    REQUIRE(s.open(
        dev.Get(), o,
        [&](const CapturePacket& p) {
            sample = p.interleaved[0];
            frames += p.frames;
        },
        error));
    CHECK(s.deviceChannels() == 2);
    REQUIRE(s.start(error));
    REQUIRE(waitFor([&] { return frames.load() == 3 * audio.packetFrames; }));
    CHECK(sample.load() == 1.5f);  // (1 + 2) / 2
    CHECK(s.lastError().empty());
    s.close();
}

TEST_CASE("CaptureStream survives device loss", "[wasapi]") {
    FakeAudio audio;
    audio.streamError = AUDCLNT_E_DEVICE_INVALIDATED;
    const auto dev = makeFakeDevice(audio);
    const CaptureStream::Options o;
    std::string error;
    const auto ignore = [](const CapturePacket&) {};

    // До исправления каждый из вариантов заканчивался std::terminate: поток
    // завершался сам, а stop() не собирал его.
    SECTION("close and reopen, as Engine restarts") {
        CaptureStream s;
        REQUIRE(s.open(dev.Get(), o, ignore, error));
        REQUIRE(s.start(error));
        REQUIRE(waitFor([&] { return !s.lastError().empty(); }));
        CHECK(s.lastError().find("AUDCLNT_E_DEVICE_INVALIDATED") != std::string::npos);
        s.close();

        audio.streamError = S_OK;
        REQUIRE(s.open(dev.Get(), o, ignore, error));
        REQUIRE(s.start(error));
        CHECK(s.lastError().empty());
        s.close();
    }
    SECTION("start again on the same stream") {
        CaptureStream s;
        REQUIRE(s.open(dev.Get(), o, ignore, error));
        REQUIRE(s.start(error));
        REQUIRE(waitFor([&] { return !s.lastError().empty(); }));
        audio.streamError = S_OK;
        REQUIRE(s.start(error));
        CHECK(s.lastError().empty());
        CHECK(audio.starts.load() == 2);
    }
    SECTION("destroy after the thread died") {
        CaptureStream s;
        REQUIRE(s.open(dev.Get(), o, ignore, error));
        REQUIRE(s.start(error));
        REQUIRE(waitFor([&] { return !s.lastError().empty(); }));
    }
}

TEST_CASE("RenderStream prefills to target, renders, and reports device loss", "[wasapi]") {
    FakeAudio audio;
    const auto dev = makeFakeDevice(audio);
    std::atomic<uint32_t> requested{0};
    RenderStream r;
    RenderStream::Options o;
    o.targetMs = 20;
    std::string error;
    REQUIRE(r.open(dev.Get(), o, [&](float*, uint32_t frames) { requested += frames; }, error));
    CHECK(r.targetFrames() == 960);
    REQUIRE(r.start(error));
    // Предзаполнение тишиной до цели, затем событие Start: колбэк дозаполняет ещё раз
    // (поддельный padding всегда 0).
    REQUIRE(waitFor([&] { return audio.renderedFrames.load() == 2 * 960; }));
    CHECK(requested.load() == 960);

    audio.streamError = AUDCLNT_E_DEVICE_INVALIDATED;
    audio.signal();
    REQUIRE(waitFor([&] { return !r.lastError().empty(); }));
    // Устройство всё ещё недоступно: повторный старт честно отказывает на предзаполнении.
    CHECK_FALSE(r.start(error));
    CHECK(error.find("prefill") != std::string::npos);
    r.close();
}
