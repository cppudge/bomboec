#include "engine/recorder.h"

#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace bomboec;

TEST_CASE("Recorder writes tracks pushed from another thread", "[recorder]") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / ("bomboec-recorder-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    Recorder rec;
    std::string error;
    REQUIRE(rec.open(dir, {{"mono.wav", 1}, {"stereo.wav", 2}}, 48000, error));
    constexpr uint32_t kFrames = 480, kPushes = 50;
    std::thread audio([&] {
        std::vector<float> mono(kFrames), stereo(2 * kFrames);
        for (uint32_t p = 0; p < kPushes; ++p) {
            for (uint32_t i = 0; i < kFrames; ++i) {
                const float v = float(p * kFrames + i) / 100000.0f;
                mono[i] = v;
                stereo[2 * i] = v;
                stereo[2 * i + 1] = -v;
            }
            rec.push(0, mono.data(), kFrames);
            rec.push(1, stereo.data(), kFrames);
            rec.push(2, mono.data(), kFrames);  // несуществующая дорожка: молча игнорируется
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    audio.join();
    rec.close();
    CHECK(rec.dropped() == 0);

    std::vector<float> data;
    uint32_t channels = 0, rate = 0;
    REQUIRE(readWavFile(dir / "stereo.wav", data, channels, rate, error));
    CHECK(channels == 2);
    CHECK(rate == 48000);
    REQUIRE(data.size() == size_t(2 * kFrames * kPushes));
    CHECK(data[2 * 1234] == float(1234) / 100000.0f);
    CHECK(data[2 * 1234 + 1] == -float(1234) / 100000.0f);
    REQUIRE(readWavFile(dir / "mono.wav", data, channels, rate, error));
    CHECK(data.size() == size_t(kFrames * kPushes));

    std::filesystem::remove_all(dir, ec);
}
