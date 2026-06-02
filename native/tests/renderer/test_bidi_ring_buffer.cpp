#include <catch2/catch_test_macros.hpp>
#include "renderer/buffer/bidi_ring_buffer.h"
#include <deque>
#include <random>

using namespace vr;

namespace {

TextureFrame make_frame(int64_t pts) {
    TextureFrame frame;
    frame.pts_us = pts;
    frame.duration_us = 33'333;
    return frame;
}

void require_matches_model(const BidiRingBuffer& brb,
                           const std::deque<int64_t>& readable,
                           size_t retreated) {
    REQUIRE(brb.total_count() == readable.size());
    REQUIRE(brb.empty() == readable.empty());
    REQUIRE(brb.forward_count() == readable.size());
    REQUIRE(brb.backward_count() == retreated);

    if (readable.empty()) {
        REQUIRE_FALSE(brb.peek().has_value());
        REQUIRE_FALSE(brb.can_advance());
        return;
    }

    REQUIRE(brb.can_advance());
    for (size_t i = 0; i < readable.size(); ++i) {
        auto frame = brb.peek(static_cast<int>(i));
        REQUIRE(frame.has_value());
        REQUIRE(frame->pts_us == readable[i]);
    }
    REQUIRE_FALSE(brb.peek(static_cast<int>(readable.size())).has_value());
}

} // namespace

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

TEST_CASE("BidiRingBuffer: deterministic operation sequence preserves model",
          "[bidi_ring_buffer]") {
    constexpr size_t kForwardDepth = 5;
    constexpr size_t kBackwardDepth = 3;
    BidiRingBuffer brb(kForwardDepth, kBackwardDepth);
    std::deque<int64_t> readable;
    std::deque<int64_t> backward;
    std::mt19937 rng(0xB1D1);
    int64_t next_pts = 1000;
    size_t retreated = 0;

    for (int step = 0; step < 1000; ++step) {
        const int op = static_cast<int>(rng() % 4);
        if (op <= 1) {
            const bool expected = readable.size() < brb.max_count();
            const bool pushed = brb.push(make_frame(next_pts));
            REQUIRE(pushed == expected);
            if (pushed) {
                readable.push_back(next_pts);
                next_pts += 1000;
            }
        } else if (op == 2) {
            const bool expected = !readable.empty();
            REQUIRE(brb.advance() == expected);
            if (expected) {
                backward.push_back(readable.front());
                if (backward.size() > kBackwardDepth) {
                    backward.pop_front();
                }
                readable.pop_front();
                if (retreated > 0) {
                    --retreated;
                }
            }
        } else {
            brb.clear();
            readable.clear();
            backward.clear();
            retreated = 0;
        }

        require_matches_model(brb, readable, retreated);
    }
}

TEST_CASE("BidiRingBuffer: retreat re-exposes the bounded backward window",
          "[bidi_ring_buffer]") {
    BidiRingBuffer brb(4, 2);
    REQUIRE(brb.push(make_frame(1000)));
    REQUIRE(brb.push(make_frame(2000)));
    REQUIRE(brb.push(make_frame(3000)));

    REQUIRE(brb.advance());
    REQUIRE(brb.peek()->pts_us == 2000);
    REQUIRE(brb.advance());
    REQUIRE(brb.peek()->pts_us == 3000);

    REQUIRE(brb.can_retreat());
    REQUIRE(brb.available_retreat_count() == 2);
    REQUIRE(brb.retreat());
    REQUIRE(brb.peek()->pts_us == 2000);
    REQUIRE(brb.backward_count() == 1);
    REQUIRE(brb.available_retreat_count() == 1);

    REQUIRE(brb.can_retreat());
    REQUIRE(brb.retreat());
    REQUIRE(brb.peek()->pts_us == 1000);
    REQUIRE(brb.backward_count() == 2);
    REQUIRE(brb.available_retreat_count() == 0);
    REQUIRE_FALSE(brb.can_retreat());

    REQUIRE(brb.advance());
    REQUIRE(brb.peek()->pts_us == 2000);
    REQUIRE(brb.backward_count() == 1);
    REQUIRE(brb.available_retreat_count() == 1);
}
