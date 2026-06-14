#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "renderer/sync/render_sink.h"
#include "renderer/render/presentation_scheduler.h"
#include "renderer/clock.h"
#include "renderer/buffer/track_buffer.h"

#include <memory>

using namespace vr;
using MockTimeSource = vr::test::MockTimeSource;

TEST_CASE("RenderSink: no tracks returns should_present=false", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    RenderSink sink(clock);
    PresentDecision decision = sink.evaluate();

    REQUIRE(decision.should_present == false);
}

TEST_CASE("RenderSink: single track with matching PTS presents", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track = std::make_shared<TrackBuffer>(4, 2);
    TextureFrame frame;
    frame.pts_us = 1000000;
    frame.duration_us = 33000; // ~30fps
    frame.texture_handle = reinterpret_cast<void*>(0x1);
    track->push_frame(frame);

    RenderSink sink(clock);
    sink.set_track(0, track);

    // Advance mock clock to frame PTS
    mt.t = 1000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[0]->pts_us == 1000000);
}

TEST_CASE("RenderSink: present decisions carry track identity",
          "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track = std::make_shared<TrackBuffer>(4, 2);
    TextureFrame frame;
    frame.pts_us = 1000000;
    frame.duration_us = 33000;
    frame.texture_handle = reinterpret_cast<void*>(0x1);
    track->push_frame(frame);

    RenderSink sink(clock);
    sink.set_track(0, track, 42, 7);

    mt.t = 1000000;
    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.file_ids[0] == 42);
    REQUIRE(decision.track_generations[0] == 7);

    sink.set_track(0, nullptr);
    decision = sink.evaluate();
    REQUIRE(decision.file_ids[0] == -1);
    REQUIRE(decision.track_generations[0] == 0);
}

TEST_CASE("PresentationScheduler: held still frame does not mask newer video track",
          "[presentation_scheduler]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto still = std::make_shared<TrackBuffer>(4, 2);
    TextureFrame still_frame;
    still_frame.pts_us = 0;
    still_frame.duration_us = 1000000;
    still_frame.texture_handle = reinterpret_cast<void*>(0x1);
    still->push_frame(still_frame);

    auto video = std::make_shared<TrackBuffer>(4, 2);
    TextureFrame first_video_frame;
    first_video_frame.pts_us = 1000000;
    first_video_frame.duration_us = 1000000;
    first_video_frame.texture_handle = reinterpret_cast<void*>(0x2);
    video->push_frame(first_video_frame);
    TextureFrame second_video_frame = first_video_frame;
    second_video_frame.pts_us = 2000000;
    second_video_frame.texture_handle = reinterpret_cast<void*>(0x3);
    video->push_frame(second_video_frame);

    RenderSink sink(clock);
    sink.set_track(0, still, 10, 1);
    sink.set_track(1, video, 20, 1);

    PresentationScheduler scheduler;
    mt.t = 1000000;
    auto tick = scheduler.tick(sink);
    REQUIRE(tick.has_presentable_frame);
    REQUIRE(tick.should_notify);
    REQUIRE(tick.selected_pts_us == 1000000);

    mt.t = 2000000;
    tick = scheduler.tick(sink);
    REQUIRE(tick.has_presentable_frame);
    REQUIRE(tick.should_notify);
    REQUIRE(tick.selected_pts_us == 2000000);
}

TEST_CASE("RenderSink: registered track is lifetime pinned by the sink",
          "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    RenderSink sink(clock);
    std::weak_ptr<TrackBuffer> weak_track;
    {
        auto track = std::make_shared<TrackBuffer>(4, 2);
        weak_track = track;

        TextureFrame frame;
        frame.pts_us = 1000000;
        frame.duration_us = 33000;
        frame.texture_handle = reinterpret_cast<void*>(0x1);
        track->push_frame(frame);

        sink.set_track(0, track);
    }

    REQUIRE_FALSE(weak_track.expired());

    mt.t = 1000000;
    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[0]->pts_us == 1000000);

    sink.set_track(0, nullptr);
    REQUIRE(weak_track.expired());
}

TEST_CASE("RenderSink: single track with future PTS does not present", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track = std::make_shared<TrackBuffer>(4, 2);
    TextureFrame frame;
    frame.pts_us = 3000000;
    frame.duration_us = 33000;
    frame.texture_handle = reinterpret_cast<void*>(0x2);
    track->push_frame(frame);

    RenderSink sink(clock);
    sink.set_track(0, track);

    // Clock at 2000000, frame at 3000000 - frame is in the future
    mt.t = 2000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == false);
}

TEST_CASE("RenderSink: expired frames advance to the stable tail frame", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track = std::make_shared<TrackBuffer>(4, 2);

    // Frame at 1000000 with duration 33000
    TextureFrame frame1;
    frame1.pts_us = 1000000;
    frame1.duration_us = 33000;
    frame1.texture_handle = reinterpret_cast<void*>(0x1);
    track->push_frame(frame1);

    // Frame at 1033000
    TextureFrame frame2;
    frame2.pts_us = 1033000;
    frame2.duration_us = 33000;
    frame2.texture_handle = reinterpret_cast<void*>(0x2);
    track->push_frame(frame2);

    RenderSink sink(clock);
    sink.set_track(0, track);

    // Clock at 2000000, frame1 is expired by frame2's PTS. With no following
    // PTS, frame2 is the stable tail frame and remains visible.
    mt.t = 2000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[0]->pts_us == 1033000);
}

TEST_CASE("RenderSink: sparse single frame remains visible after nominal duration",
          "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track = std::make_shared<TrackBuffer>(4, 2);

    TextureFrame frame;
    frame.pts_us = 0;
    frame.duration_us = 1000000;
    frame.texture_handle = reinterpret_cast<void*>(0x1);
    track->push_frame(frame);

    RenderSink sink(clock);
    sink.set_track(0, track);

    mt.t = 2000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[0]->pts_us == 0);
}

TEST_CASE("RenderSink: next PTS defines display window when duration metadata is bogus",
          "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track = std::make_shared<TrackBuffer>(4, 2);

    TextureFrame frame1;
    frame1.pts_us = 1000000;
    frame1.duration_us = 100;
    frame1.texture_handle = reinterpret_cast<void*>(0x1);
    track->push_frame(frame1);

    TextureFrame frame2;
    frame2.pts_us = 1033333;
    frame2.duration_us = 66666;
    frame2.texture_handle = reinterpret_cast<void*>(0x2);
    track->push_frame(frame2);

    RenderSink sink(clock);
    sink.set_track(0, track);

    mt.t = 1010000;
    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[0]->pts_us == 1000000);

    mt.t = 1033333;
    decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[0]->pts_us == 1033333);
}

TEST_CASE("RenderSink: two tracks both ready present", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track1 = std::make_shared<TrackBuffer>(4, 2);
    auto track2 = std::make_shared<TrackBuffer>(4, 2);

    TextureFrame frame1;
    frame1.pts_us = 1000000;
    frame1.duration_us = 33000;
    frame1.texture_handle = reinterpret_cast<void*>(0x1);
    track1->push_frame(frame1);

    TextureFrame frame2;
    frame2.pts_us = 1000000;
    frame2.duration_us = 33000;
    frame2.texture_handle = reinterpret_cast<void*>(0x2);
    track2->push_frame(frame2);

    RenderSink sink(clock);
    sink.set_track(0, track1);
    sink.set_track(1, track2);

    mt.t = 1000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[1].has_value());
    REQUIRE(decision.frames[0]->pts_us == 1000000);
    REQUIRE(decision.frames[1]->pts_us == 1000000);
}

TEST_CASE("RenderSink: two tracks within tolerance present", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track1 = std::make_shared<TrackBuffer>(4, 2);
    auto track2 = std::make_shared<TrackBuffer>(4, 2);

    // Track 1 at 1000000
    TextureFrame frame1;
    frame1.pts_us = 1000000;
    frame1.duration_us = 33000;
    frame1.texture_handle = reinterpret_cast<void*>(0x1);
    track1->push_frame(frame1);

    // Track 2 at 1003000 (3ms later, within 5ms tolerance)
    TextureFrame frame2;
    frame2.pts_us = 1003000;
    frame2.duration_us = 33000;
    frame2.texture_handle = reinterpret_cast<void*>(0x2);
    track2->push_frame(frame2);

    RenderSink sink(clock);
    sink.set_track(0, track1);
    sink.set_track(1, track2);

    mt.t = 1000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[1].has_value());
}

TEST_CASE("RenderSink: PTS tolerance boundary is inclusive", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track = std::make_shared<TrackBuffer>(4, 2);
    TextureFrame frame;
    frame.pts_us = 1000000 + kRenderSinkPtsToleranceUs;
    frame.duration_us = 33000;
    frame.texture_handle = reinterpret_cast<void*>(0x1);
    track->push_frame(frame);

    RenderSink sink(clock);
    sink.set_track(0, track);
    mt.t = 1000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
}

TEST_CASE("RenderSink: independent present when tracks have different timing", "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track1 = std::make_shared<TrackBuffer>(4, 2);
    auto track2 = std::make_shared<TrackBuffer>(4, 2);

    // Track 1 at 1000000
    TextureFrame frame1;
    frame1.pts_us = 1000000;
    frame1.duration_us = 33000;
    frame1.texture_handle = reinterpret_cast<void*>(0x1);
    track1->push_frame(frame1);

    // Track 2 at 1010000 (10ms later, outside 5ms tolerance)
    TextureFrame frame2;
    frame2.pts_us = 1010000;
    frame2.duration_us = 33000;
    frame2.texture_handle = reinterpret_cast<void*>(0x2);
    track2->push_frame(frame2);

    RenderSink sink(clock);
    sink.set_track(0, track1);
    sink.set_track(1, track2);

    mt.t = 1000000;

    PresentDecision decision = sink.evaluate();
    // Track 1 is in display window → should_present = true (any-ready)
    // Track 2 is 10ms in the future → nullopt (filled from last_decision_ by renderer)
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(!decision.frames[1].has_value());
}
