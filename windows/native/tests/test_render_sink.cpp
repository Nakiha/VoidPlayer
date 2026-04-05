#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "video_renderer/sync/render_sink.h"
#include "video_renderer/clock.h"
#include "video_renderer/buffer/track_buffer.h"

using namespace vr;
using MockTimeSource = vr::test::MockTimeSource;

TEST_CASE("RenderSink: no tracks returns should_present=false", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    RenderSink sink(clock);
    PresentDecision decision = sink.evaluate();

    REQUIRE(decision.should_present == false);
    REQUIRE(decision.frames.empty());
}

TEST_CASE("RenderSink: single track with matching PTS presents", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    TrackBuffer track(4, 2);
    TextureFrame frame;
    frame.pts_us = 1000000;
    frame.duration_us = 33000; // ~30fps
    frame.texture_handle = reinterpret_cast<void*>(0x1);
    track.push_frame(frame);

    RenderSink sink(clock);
    sink.add_track(&track);

    // Advance mock clock to frame PTS
    mt.t = 1000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames.size() == 1);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[0]->pts_us == 1000000);
}

TEST_CASE("RenderSink: single track with future PTS does not present", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    TrackBuffer track(4, 2);
    TextureFrame frame;
    frame.pts_us = 3000000;
    frame.duration_us = 33000;
    frame.texture_handle = reinterpret_cast<void*>(0x2);
    track.push_frame(frame);

    RenderSink sink(clock);
    sink.add_track(&track);

    // Clock at 2000000, frame at 3000000 - frame is in the future
    mt.t = 2000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == false);
}

TEST_CASE("RenderSink: expired frame is advanced past", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    TrackBuffer track(4, 2);

    // Frame at 1000000 with duration 33000
    TextureFrame frame1;
    frame1.pts_us = 1000000;
    frame1.duration_us = 33000;
    frame1.texture_handle = reinterpret_cast<void*>(0x1);
    track.push_frame(frame1);

    // Frame at 1033000
    TextureFrame frame2;
    frame2.pts_us = 1033000;
    frame2.duration_us = 33000;
    frame2.texture_handle = reinterpret_cast<void*>(0x2);
    track.push_frame(frame2);

    RenderSink sink(clock);
    sink.add_track(&track);

    // Clock at 2000000, both frames are expired (frame1 ends at 1033000)
    mt.t = 2000000;

    PresentDecision decision = sink.evaluate();
    // Both frames expired, no frames available to display
    REQUIRE(decision.should_present == false);
}

TEST_CASE("RenderSink: two tracks both ready present", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    TrackBuffer track1(4, 2);
    TrackBuffer track2(4, 2);

    TextureFrame frame1;
    frame1.pts_us = 1000000;
    frame1.duration_us = 33000;
    frame1.texture_handle = reinterpret_cast<void*>(0x1);
    track1.push_frame(frame1);

    TextureFrame frame2;
    frame2.pts_us = 1000000;
    frame2.duration_us = 33000;
    frame2.texture_handle = reinterpret_cast<void*>(0x2);
    track2.push_frame(frame2);

    RenderSink sink(clock);
    sink.add_track(&track1);
    sink.add_track(&track2);

    mt.t = 1000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames.size() == 2);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[1].has_value());
    REQUIRE(decision.frames[0]->pts_us == 1000000);
    REQUIRE(decision.frames[1]->pts_us == 1000000);
}

TEST_CASE("RenderSink: two tracks within tolerance present", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    TrackBuffer track1(4, 2);
    TrackBuffer track2(4, 2);

    // Track 1 at 1000000
    TextureFrame frame1;
    frame1.pts_us = 1000000;
    frame1.duration_us = 33000;
    frame1.texture_handle = reinterpret_cast<void*>(0x1);
    track1.push_frame(frame1);

    // Track 2 at 1003000 (3ms later, within 5ms tolerance)
    TextureFrame frame2;
    frame2.pts_us = 1003000;
    frame2.duration_us = 33000;
    frame2.texture_handle = reinterpret_cast<void*>(0x2);
    track2.push_frame(frame2);

    RenderSink sink(clock);
    sink.add_track(&track1);
    sink.add_track(&track2);

    mt.t = 1000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[1].has_value());
}

TEST_CASE("RenderSink: independent present when tracks have different timing", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    TrackBuffer track1(4, 2);
    TrackBuffer track2(4, 2);

    // Track 1 at 1000000
    TextureFrame frame1;
    frame1.pts_us = 1000000;
    frame1.duration_us = 33000;
    frame1.texture_handle = reinterpret_cast<void*>(0x1);
    track1.push_frame(frame1);

    // Track 2 at 1010000 (10ms later, outside 5ms tolerance)
    TextureFrame frame2;
    frame2.pts_us = 1010000;
    frame2.duration_us = 33000;
    frame2.texture_handle = reinterpret_cast<void*>(0x2);
    track2.push_frame(frame2);

    RenderSink sink(clock);
    sink.add_track(&track1);
    sink.add_track(&track2);

    mt.t = 1000000;

    PresentDecision decision = sink.evaluate();
    // Track 1 is in display window → should_present = true (any-ready)
    // Track 2 is 10ms in the future → nullopt (filled from last_decision_ by renderer)
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(!decision.frames[1].has_value());
}
