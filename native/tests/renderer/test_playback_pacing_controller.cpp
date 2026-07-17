#include <catch2/catch_test_macros.hpp>

#include "playback/playback_controller.h"
#include "renderer/playback/playback_pacing_controller.h"

#include <memory>

using namespace vr;

namespace {

class PacingAudioOutput final : public AudioOutput {
public:
    bool add_track(int,
                   PacketQueue&,
                   const AVCodecParameters*,
                   AVRational) override {
        return true;
    }
    void remove_track(int) override {}
    void clear() override {}
    void play() override { ++play_count; }
    void pause() override { ++pause_count; }
    void set_active_track(int file_id) override {
        active_track_id = file_id;
    }
    int active_track() const override { return active_track_id; }
    void set_track_decode_paused(int, bool) override {}
    void set_all_decode_paused(bool) override {}
    void notify_seek(int, int64_t, SeekType) override {}
    AudioOutputStats stats() const override { return {}; }

    int play_count = 0;
    int pause_count = 0;
    int active_track_id = -1;
};

} // namespace

TEST_CASE("PlaybackPacingController holds and recovers exact playback",
          "[playback_pacing]") {
    PlaybackPacingController controller;
    PlaybackPacingSnapshot healthy;
    healthy.has_active_tracks = true;
    healthy.frontier_limited = true;
    healthy.resume_ready = true;
    healthy.bottleneck_buffered_frames = 3;
    healthy.bottleneck_target_frames = 3;
    healthy.headroom_us = 80000;
    healthy.high_watermark_us = 60000;

    auto running = controller.evaluate({true, false, true, 1.0, 0, healthy});
    REQUIRE(running.state == PlaybackPacingState::Running);
    REQUIRE_FALSE(running.hold_for_pacing);
    REQUIRE(running.effective_speed == 1.0);

    auto starved = healthy;
    starved.starvation_risk = true;
    starved.resume_ready = false;
    starved.headroom_us = 0;
    auto hold = controller.evaluate({true, false, true, 1.0, 100, starved});
    REQUIRE(hold.state == PlaybackPacingState::Rebuffering);
    REQUIRE(hold.entered_rebuffering);
    REQUIRE(hold.hold_for_pacing);

    auto still_waiting =
        controller.evaluate({true, true, true, 1.0, 200, starved});
    REQUIRE(still_waiting.state == PlaybackPacingState::Rebuffering);
    REQUIRE_FALSE(still_waiting.release_pacing_hold);

    healthy.headroom_us = 30000;
    auto recovered =
        controller.evaluate({true, true, true, 1.0, 500, healthy});
    REQUIRE(recovered.state == PlaybackPacingState::Running);
    REQUIRE(recovered.resumed_running);
    REQUIRE(recovered.release_pacing_hold);
    REQUIRE(recovered.resume_decode);
    REQUIRE(recovered.effective_speed == 1.0);
    REQUIRE(controller.diagnostics().rebuffer_count == 1);
    REQUIRE(controller.diagnostics().rebuffer_duration_us == 400);
}

TEST_CASE("PlaybackPacingController does not scale a full shallow buffer by frame phase",
          "[playback_pacing]") {
    PlaybackPacingController controller;
    PlaybackPacingSnapshot snapshot;
    snapshot.has_active_tracks = true;
    snapshot.frontier_limited = true;
    snapshot.resume_ready = true;
    snapshot.min_buffered_frames = 2;
    snapshot.bottleneck_buffered_frames = 2;
    snapshot.bottleneck_target_frames = 2;
    snapshot.headroom_us = 16667;
    snapshot.high_watermark_us = 33333;

    auto half_frame_remaining =
        controller.evaluate({true, false, true, 2.0, 0, snapshot});
    REQUIRE(half_frame_remaining.state == PlaybackPacingState::Running);
    REQUIRE(half_frame_remaining.update_effective_speed);
    REQUIRE(half_frame_remaining.effective_speed == 2.0);

    snapshot.headroom_us = 1000;
    auto near_frame_boundary =
        controller.evaluate({true, false, true, 2.0, 1, snapshot});
    REQUIRE(near_frame_boundary.effective_speed == 2.0);
}

TEST_CASE("PlaybackPacingController scales only from stable forward occupancy",
          "[playback_pacing]") {
    PlaybackPacingController controller;
    PlaybackPacingSnapshot snapshot;
    snapshot.has_active_tracks = true;
    snapshot.frontier_limited = true;
    snapshot.resume_ready = true;
    snapshot.bottleneck_buffered_frames = 2;
    snapshot.bottleneck_target_frames = 3;
    snapshot.headroom_us = 59000;
    snapshot.high_watermark_us = 60000;

    auto one_of_two_ahead =
        controller.evaluate({true, false, true, 2.0, 0, snapshot});
    REQUIRE(one_of_two_ahead.state == PlaybackPacingState::Running);
    REQUIRE(one_of_two_ahead.update_effective_speed);
    REQUIRE(one_of_two_ahead.effective_speed == 1.25);

    snapshot.bottleneck_buffered_frames = 3;
    snapshot.headroom_us = 1000;
    auto target_reached =
        controller.evaluate({true, false, true, 2.0, 1, snapshot});
    REQUIRE(target_reached.effective_speed == 2.0);
}

TEST_CASE("PlaybackPacingController uses hold-only pacing with audible audio",
          "[playback_pacing]") {
    PlaybackPacingController controller;
    PlaybackPacingSnapshot snapshot;
    snapshot.has_active_tracks = true;
    snapshot.frontier_limited = true;
    snapshot.resume_ready = true;
    snapshot.headroom_us = 10000;
    snapshot.high_watermark_us = 60000;

    auto decision =
        controller.evaluate({true, false, false, 1.0, 0, snapshot});
    REQUIRE(decision.state == PlaybackPacingState::Running);
    REQUIRE(decision.effective_speed == 1.0);
}

TEST_CASE("PlaybackPacingController preserves pacing hold across user pause",
          "[playback_pacing]") {
    PlaybackPacingController controller;
    PlaybackPacingSnapshot starved;
    starved.has_active_tracks = true;
    starved.starvation_risk = true;
    starved.frontier_limited = true;

    auto hold =
        controller.evaluate({true, false, true, 1.0, 100, starved});
    REQUIRE(hold.hold_for_pacing);

    auto paused =
        controller.evaluate({false, true, true, 1.0, 200, starved});
    REQUIRE(paused.state == PlaybackPacingState::Rebuffering);

    auto recovered = starved;
    recovered.starvation_risk = false;
    recovered.resume_ready = true;
    recovered.headroom_us = 60000;
    recovered.high_watermark_us = 60000;
    auto resumed =
        controller.evaluate({true, true, true, 1.0, 300, recovered});
    REQUIRE(resumed.release_pacing_hold);
    REQUIRE(resumed.state == PlaybackPacingState::Running);
}

TEST_CASE("PlaybackController pacing hold gates clock and audio together",
          "[playback_pacing][audio]") {
    PacingAudioOutput* audio = nullptr;
    PlaybackController playback([&audio]() {
        auto output = std::make_unique<PacingAudioOutput>();
        audio = output.get();
        return output;
    });
    playback.start_session();
    REQUIRE(audio != nullptr);

    playback.play();
    REQUIRE_FALSE(playback.clock().is_paused());
    REQUIRE(audio->play_count == 1);

    playback.hold_for_pacing();
    REQUIRE(playback.pacing_held());
    REQUIRE(playback.clock().is_paused());
    REQUIRE(audio->pause_count == 1);

    playback.play();
    REQUIRE(playback.clock().is_paused());
    REQUIRE(audio->play_count == 1);

    playback.release_pacing_hold();
    REQUIRE_FALSE(playback.pacing_held());
    REQUIRE_FALSE(playback.clock().is_paused());
    REQUIRE(audio->play_count == 2);
}
