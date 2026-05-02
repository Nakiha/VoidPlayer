#include <catch2/catch_test_macros.hpp>
#include "video_renderer/buffer/track_buffer.h"

using namespace vr;

TEST_CASE("TrackBuffer: initial state is Empty and peek returns nullopt",
          "[track_buffer]") {
    TrackBuffer tb;

    REQUIRE(tb.state() == TrackState::Empty);
    REQUIRE_FALSE(tb.peek(0).has_value());
    REQUIRE(tb.last_presented_pts_us() == 0);
}

TEST_CASE("TrackBuffer: transition to Buffering after push_frame",
          "[track_buffer]") {
    TrackBuffer tb;
    REQUIRE(tb.state() == TrackState::Empty);

    TextureFrame f1;
    f1.pts_us = 1000;
    f1.duration_us = 33000;
    tb.push_frame(f1);
    tb.set_state(TrackState::Buffering);

    REQUIRE(tb.state() == TrackState::Buffering);
}

TEST_CASE("TrackBuffer: push_frame and peek work together",
          "[track_buffer]") {
    TrackBuffer tb;

    TextureFrame f1;
    f1.pts_us = 1000;
    f1.duration_us = 33000;
    f1.is_ref = true;
    f1.texture_handle = reinterpret_cast<void*>(0x1);

    TextureFrame f2;
    f2.pts_us = 34000;
    f2.duration_us = 33000;

    tb.push_frame(f1);
    tb.push_frame(f2);

    auto result = tb.peek(0);
    REQUIRE(result.has_value());
    REQUIRE(result->pts_us == 1000);
    REQUIRE(result->duration_us == 33000);
    REQUIRE(result->is_ref == true);
    REQUIRE(result->texture_handle == reinterpret_cast<void*>(0x1));

    auto result2 = tb.peek(1);
    REQUIRE(result2.has_value());
    REQUIRE(result2->pts_us == 34000);
}

TEST_CASE("TrackBuffer: advance updates last_presented_pts_us",
          "[track_buffer]") {
    TrackBuffer tb;

    TextureFrame f1;
    f1.pts_us = 1000;
    TextureFrame f2;
    f2.pts_us = 34000;
    TextureFrame f3;
    f3.pts_us = 67000;

    tb.push_frame(f1);
    tb.push_frame(f2);
    tb.push_frame(f3);

    // Before advance, last_presented_pts_us is 0
    REQUIRE(tb.last_presented_pts_us() == 0);

    // peek(0) should be f1 (pts=1000). After advance, last_presented
    // becomes 1000 (the frame we were on before moving forward).
    REQUIRE(tb.peek(0)->pts_us == 1000);
    REQUIRE(tb.advance());

    REQUIRE(tb.last_presented_pts_us() == 1000);

    // Now peek(0) should be f2
    REQUIRE(tb.peek(0)->pts_us == 34000);

    // Advance again -> last_presented becomes f2's pts
    REQUIRE(tb.advance());
    REQUIRE(tb.last_presented_pts_us() == 34000);

    // peek(0) is now f3
    REQUIRE(tb.peek(0)->pts_us == 67000);
}

TEST_CASE("TrackBuffer: state transitions through all states",
          "[track_buffer]") {
    TrackBuffer tb;

    // Empty -> Buffering
    REQUIRE(tb.state() == TrackState::Empty);
    tb.set_state(TrackState::Buffering);
    REQUIRE(tb.state() == TrackState::Buffering);

    // Buffering -> Ready
    tb.set_state(TrackState::Ready);
    REQUIRE(tb.state() == TrackState::Ready);

    // Ready -> Flushing
    tb.set_state(TrackState::Flushing);
    REQUIRE(tb.state() == TrackState::Flushing);

    // Flushing -> Empty
    tb.set_state(TrackState::Empty);
    REQUIRE(tb.state() == TrackState::Empty);

    // Also verify Error state
    tb.set_state(TrackState::Error);
    REQUIRE(tb.state() == TrackState::Error);
}

TEST_CASE("TrackBuffer: set_last_presented_pts_us stores value",
          "[track_buffer]") {
    TrackBuffer tb;

    REQUIRE(tb.last_presented_pts_us() == 0);

    tb.set_last_presented_pts_us(123456);
    REQUIRE(tb.last_presented_pts_us() == 123456);

    tb.set_last_presented_pts_us(999999);
    REQUIRE(tb.last_presented_pts_us() == 999999);
}

TEST_CASE("TrackBuffer: reset clears frames, state, and last presented PTS",
          "[track_buffer]") {
    TrackBuffer tb;
    TextureFrame frame;
    frame.pts_us = 42000;

    tb.push_frame(frame);
    REQUIRE(tb.advance());
    tb.set_state(TrackState::Ready);
    REQUIRE(tb.last_presented_pts_us() == 42000);

    tb.reset();

    REQUIRE(tb.state() == TrackState::Empty);
    REQUIRE(tb.last_presented_pts_us() == 0);
    REQUIRE(tb.total_count() == 0);
    REQUIRE_FALSE(tb.peek(0).has_value());
}
