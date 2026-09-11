#include "core/wav.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <vector>

using namespace bomboec;

TEST_CASE("WavWriter/WavReader float32 round-trip") {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "bomboec_test_rt.wav";
    std::vector<float> data(2 * 1000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = float(i) / 1000.0f - 1.0f;

    std::string error;
    {
        WavWriter w;
        REQUIRE(w.open(path, 2, 48000, error));
        CHECK(w.write(data.data(), 600) == 600);
        CHECK(w.write(data.data() + 600 * 2, 400) == 400);
        CHECK(w.framesWritten() == 1000);
    }

    std::vector<float> back;
    uint32_t channels = 0, rate = 0;
    REQUIRE(readWavFile(path, back, channels, rate, error));
    CHECK(channels == 2);
    CHECK(rate == 48000);
    REQUIRE(back.size() == data.size());
    CHECK(back[0] == data[0]);
    CHECK(back[1999] == data[1999]);

    std::filesystem::remove(path);
    CHECK_FALSE(readWavFile(path, back, channels, rate, error));
}
