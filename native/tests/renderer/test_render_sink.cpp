#include <catch2/catch_test_macros.hpp>
#include "test_utils.h"
#include "renderer/sync/render_sink.h"
#include "renderer/render/presentation_scheduler.h"
#include "renderer/clock.h"
#include "renderer/buffer/track_buffer.h"

#include <memory>
#include <utility>

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

TEST_CASE("RenderSink: commit requires current track identity",
          "[render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track = std::make_shared<TrackBuffer>(4, 2);
    TextureFrame frame;
    frame.pts_us = 0;
    frame.texture_handle = reinterpret_cast<void*>(0x1);
    track->push_frame(frame);

    RenderSink sink(clock);
    sink.set_track(0, track, 10, 7);
    auto decision = sink.evaluate();
    REQUIRE(decision.frames[0].has_value());

    decision.track_generations[0] = 8;
    REQUIRE(sink.commit_presented(decision) == 0);
    REQUIRE(track->total_count() == 1);

    decision.track_generations[0] = 7;
    REQUIRE(sink.commit_presented(decision) == 1);
    REQUIRE(track->total_count() == 0);
}

TEST_CASE("PresentationScheduler distinguishes equal-PTS decoded frames",
          "[presentation_scheduler][render_sink]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track = std::make_shared<TrackBuffer>(4, 2);
    TextureFrame first;
    first.pts_us = 0;
    first.texture_handle = reinterpret_cast<void*>(0x1);
    track->push_frame(first);
    TextureFrame second = first;
    second.texture_handle = reinterpret_cast<void*>(0x2);
    track->push_frame(second);

    RenderSink sink(clock);
    sink.set_track(0, track, 10, 1);
    PresentationScheduler scheduler;

    auto first_tick = scheduler.tick(sink);
    REQUIRE(first_tick.should_notify);
    scheduler.commit_presented(first_tick.decision);
    REQUIRE(sink.commit_presented(first_tick.decision) == 1);

    auto second_tick = scheduler.tick(sink);
    REQUIRE(second_tick.should_notify);
    scheduler.commit_presented(second_tick.decision);
    REQUIRE(sink.commit_presented(second_tick.decision) == 1);
}

TEST_CASE("PresentationScheduler retries a submission until it is committed",
          "[presentation_scheduler][render_sink][playback_pacing]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track = std::make_shared<TrackBuffer>(4, 2);
    TextureFrame frame;
    frame.pts_us = 0;
    frame.texture_handle = reinterpret_cast<void*>(0x1);
    track->push_frame(frame);

    RenderSink sink(clock);
    sink.set_track(0, track, 10, 1);
    PresentationScheduler scheduler;

    const auto rejected = scheduler.tick(sink);
    REQUIRE(rejected.should_notify);
    REQUIRE(track->total_count() == 1);

    const auto retry = scheduler.tick(sink);
    REQUIRE(retry.should_notify);
    scheduler.commit_presented(retry.decision);
    REQUIRE(sink.commit_presented(retry.decision) == 1);

    const auto drained = scheduler.tick(sink);
    REQUIRE_FALSE(drained.should_notify);
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
    REQUIRE(tick.selected_pts_us == 0);
    scheduler.commit_presented(tick.decision);
    REQUIRE(sink.commit_presented(tick.decision) == 1);

    tick = scheduler.tick(sink);
    REQUIRE(tick.has_presentable_frame);
    REQUIRE(tick.should_notify);
    REQUIRE(tick.selected_pts_us == 1000000);
    scheduler.commit_presented(tick.decision);
    REQUIRE(sink.commit_presented(tick.decision) == 1);

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

TEST_CASE("RenderSink: overdue frames are committed one at a time", "[render_sink]") {
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

    // Even though both frames are late, exact selection must expose frame1
    // before frame2.
    mt.t = 2000000;

    PresentDecision decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[0]->pts_us == 1000000);
    REQUIRE(track->total_count() == 2);

    REQUIRE(sink.commit_presented(decision) == 1);
    REQUIRE(track->total_count() == 1);

    decision = sink.evaluate();
    REQUIRE(decision.should_present == true);
    REQUIRE(decision.frames[0].has_value());
    REQUIRE(decision.frames[0]->pts_us == 1033000);
    REQUIRE(sink.commit_presented(decision) == 1);
    REQUIRE(track->total_count() == 0);
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
    REQUIRE(sink.commit_presented(decision) == 1);

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

TEST_CASE("RenderSink: multi-track exact events commit in global PTS order",
          "[render_sink][playback_pacing]") {
    MockTimeSource mt{0};
    Clock clock([&mt]() { return mt.t; });
    clock.play();

    auto track1 = std::make_shared<TrackBuffer>(4, 2);
    auto track2 = std::make_shared<TrackBuffer>(4, 2);
    for (const auto [pts, handle] :
         {std::pair<int64_t, uintptr_t>{33000, 1},
          {66000, 2},
          {99000, 3}}) {
        TextureFrame frame;
        frame.pts_us = pts;
        frame.texture_handle = reinterpret_cast<void*>(handle);
        track1->push_frame(frame);
    }
    for (const auto [pts, handle] :
         {std::pair<int64_t, uintptr_t>{50000, 4},
          {100000, 5}}) {
        TextureFrame frame;
        frame.pts_us = pts;
        frame.texture_handle = reinterpret_cast<void*>(handle);
        track2->push_frame(frame);
    }

    RenderSink sink(clock);
    sink.set_track(0, track1, 10, 1);
    sink.set_track(1, track2, 20, 1);
    mt.t = 100000;

    auto decision = sink.evaluate();
    REQUIRE(decision.frames[0]->pts_us == 33000);
    REQUIRE_FALSE(decision.frames[1].has_value());
    REQUIRE(sink.commit_presented(decision) == 1);

    decision = sink.evaluate();
    REQUIRE_FALSE(decision.frames[0].has_value());
    REQUIRE(decision.frames[1]->pts_us == 50000);
    REQUIRE(sink.commit_presented(decision) == 1);

    decision = sink.evaluate();
    REQUIRE(decision.frames[0]->pts_us == 66000);
    REQUIRE_FALSE(decision.frames[1].has_value());
    REQUIRE(sink.commit_presented(decision) == 1);

    decision = sink.evaluate();
    REQUIRE(decision.frames[0]->pts_us == 99000);
    REQUIRE(decision.frames[1]->pts_us == 100000);
    REQUIRE(sink.commit_presented(decision) == 2);
}
