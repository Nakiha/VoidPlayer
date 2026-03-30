#include <catch2/catch_test_macros.hpp>
#include "video_renderer/buffer/bidi_ring_buffer.h"

using namespace vr;

TEST_CASE("BidiRingBuffer: push and peek", "[bidi_ring_buffer]") {
    BidiRingBuffer brb(4, 2);

    TextureFrame f1; f1.pts_us = 1000;
    TextureFrame f2; f2.pts_us = 2000;
    TextureFrame f3; f3.pts_us = 3000;

    REQUIRE(brb.push(f1));
    REQUIRE(brb.push(f2));
    REQUIRE(brb.push(f3));

    auto result = brb.peek(0);
    REQUIRE(result.has_value());
    REQUIRE(result->pts_us == 1000);
}

TEST_CASE("BidiRingBuffer: advance moves read idx", "[bidi_ring_buffer]") {
    BidiRingBuffer brb(4, 2);

    TextureFrame f1; f1.pts_us = 1000;
    TextureFrame f2; f2.pts_us = 2000;
    TextureFrame f3; f3.pts_us = 3000;
    brb.push(f1);
    brb.push(f2);
    brb.push(f3);

    REQUIRE(brb.advance());

    auto result = brb.peek(0);
    REQUIRE(result.has_value());
    REQUIRE(result->pts_us == 2000);
}

TEST_CASE("BidiRingBuffer: cannot advance past write", "[bidi_ring_buffer]") {
    BidiRingBuffer brb(4, 2);
    TextureFrame f1; f1.pts_us = 1000;
    TextureFrame f2; f2.pts_us = 2000;
    brb.push(f1);
    brb.push(f2);

    REQUIRE(brb.advance());  // -> 2000
    REQUIRE(brb.advance());  // -> past end
    REQUIRE_FALSE(brb.can_advance());
}

TEST_CASE("BidiRingBuffer: empty buffer returns nullopt", "[bidi_ring_buffer]") {
    BidiRingBuffer brb(4, 2);
    REQUIRE(brb.empty());
    REQUIRE_FALSE(brb.peek(0).has_value());
    REQUIRE_FALSE(brb.advance());
}

TEST_CASE("BidiRingBuffer: clear resets state", "[bidi_ring_buffer]") {
    BidiRingBuffer brb(4, 2);
    brb.push({1000, 33000, 0, 0, false, nullptr});
    brb.push({2000, 33000, 0, 0, false, nullptr});
    REQUIRE_FALSE(brb.empty());

    brb.clear();
    REQUIRE(brb.empty());
    REQUIRE(brb.total_count() == 0);
}

TEST_CASE("BidiRingBuffer: peek with offset", "[bidi_ring_buffer]") {
    BidiRingBuffer brb(4, 2);

    TextureFrame f1; f1.pts_us = 1000;
    TextureFrame f2; f2.pts_us = 2000;
    TextureFrame f3; f3.pts_us = 3000;
    TextureFrame f4; f4.pts_us = 4000;
    brb.push(f1);
    brb.push(f2);
    brb.push(f3);
    brb.push(f4);

    REQUIRE(brb.peek(0)->pts_us == 1000);
    REQUIRE(brb.peek(1)->pts_us == 2000);
    REQUIRE(brb.peek(2)->pts_us == 3000);
    REQUIRE(brb.peek(3)->pts_us == 4000);
    REQUIRE_FALSE(brb.peek(4).has_value());
}

TEST_CASE("BidiRingBuffer: capacity limit", "[bidi_ring_buffer]") {
    BidiRingBuffer brb(2, 2);  // capacity = 5, usable = capacity - backward_depth = 3
    REQUIRE(brb.push({1, 0, 0, 0, false, nullptr}));
    REQUIRE(brb.push({2, 0, 0, 0, false, nullptr}));
    REQUIRE(brb.push({3, 0, 0, 0, false, nullptr}));
    REQUIRE_FALSE(brb.push({4, 0, 0, 0, false, nullptr}));  // full (backward slots reserved)
}

TEST_CASE("BidiRingBuffer: forward count", "[bidi_ring_buffer]") {
    BidiRingBuffer brb(4, 2);
    brb.push({1, 0, 0, 0, false, nullptr});
    brb.push({2, 0, 0, 0, false, nullptr});
    brb.push({3, 0, 0, 0, false, nullptr});

    REQUIRE(brb.forward_count() == 3);

    brb.advance();
    REQUIRE(brb.forward_count() == 2);
}
