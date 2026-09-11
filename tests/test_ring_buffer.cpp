#include "core/ring_buffer.h"

#include <catch2/catch_test_macros.hpp>

#include <numeric>
#include <vector>

using bomboec::RingBuffer;

TEST_CASE("RingBuffer write/read with wrap-around") {
    RingBuffer rb(2, 8);
    REQUIRE(rb.capacity() == 8);
    REQUIRE(rb.readable() == 0);
    REQUIRE(rb.writable() == 8);

    std::vector<float> in(2 * 6);
    std::iota(in.begin(), in.end(), 0.0f);
    CHECK(rb.write(in.data(), 6) == 6);
    CHECK(rb.readable() == 6);

    std::vector<float> out(2 * 4, -1.0f);
    CHECK(rb.read(out.data(), 4) == 4);
    CHECK(out[0] == 0.0f);
    CHECK(out[7] == 7.0f);
    CHECK(rb.readable() == 2);

    // Теперь запись пересекает границу буфера (позиция 6 -> 8 -> 12).
    std::vector<float> in2(2 * 6);
    std::iota(in2.begin(), in2.end(), 100.0f);
    CHECK(rb.write(in2.data(), 6) == 6);
    CHECK(rb.readable() == 8);
    CHECK(rb.writable() == 0);

    std::vector<float> out2(2 * 8);
    CHECK(rb.read(out2.data(), 8) == 8);
    CHECK(out2[0] == 8.0f);  // остаток первой записи
    CHECK(out2[3] == 11.0f);
    CHECK(out2[4] == 100.0f);  // начало второй
    CHECK(out2[15] == 111.0f);
    CHECK(rb.totalWritten() == 12);
    CHECK(rb.totalRead() == 12);
}

TEST_CASE("RingBuffer overflow is truncated, silence and discard work") {
    RingBuffer rb(1, 4);
    std::vector<float> in = {1, 2, 3, 4, 5, 6};
    CHECK(rb.write(in.data(), 6) == 4);
    CHECK(rb.readable() == 4);
    CHECK(rb.discard(2) == 2);
    CHECK(rb.writeSilence(3) == 2);
    std::vector<float> out(4, -1.0f);
    CHECK(rb.peek(out.data(), 4) == 4);
    CHECK(out[0] == 3.0f);
    CHECK(out[1] == 4.0f);
    CHECK(out[2] == 0.0f);
    CHECK(out[3] == 0.0f);
    CHECK(rb.readable() == 4);
    rb.reset();
    CHECK(rb.readable() == 0);
    CHECK(rb.totalRead() == rb.totalWritten());
}
