#include "core/frame.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using bomboec::Frame;

TEST_CASE("Frame planes and interleave round-trip") {
    Frame f(2, 4);
    REQUIRE(f.channels() == 2);
    REQUIRE(f.samples() == 4);

    const std::vector<float> interleaved = {0, 10, 1, 11, 2, 12, 3, 13};
    f.fromInterleaved(interleaved.data());
    CHECK(f.channel(0)[3] == 3.0f);
    CHECK(f.channel(1)[0] == 10.0f);
    CHECK(f.planes()[1][2] == 12.0f);

    std::vector<float> back(8, -1.0f);
    f.toInterleaved(back.data());
    CHECK(back == interleaved);

    Frame g(2, 4);
    g.copyFrom(f);
    CHECK(g.channel(1)[3] == 13.0f);
    g.clear();
    CHECK(g.channel(1)[3] == 0.0f);
}
