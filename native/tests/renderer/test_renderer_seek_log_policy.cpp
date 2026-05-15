#include <catch2/catch_test_macros.hpp>

#include "video_renderer/renderer_seek_log_policy.h"

#include <string>

using namespace vr;

TEST_CASE("RendererSeekLogPolicy: builds request and clamp log facts",
          "[renderer][seek][log]") {
    auto request = build_seek_request_log_facts(2500000, SeekType::Exact);
    REQUIRE(request.target_seconds == 2.5);
    REQUIRE(std::string(request.type_label) == "Exact");

    request = build_seek_request_log_facts(1000000, SeekType::Keyframe);
    REQUIRE(request.target_seconds == 1.0);
    REQUIRE(std::string(request.type_label) == "Keyframe");

    SeekTargetResolution resolution;
    resolution.requested_pts_us = 5000000;
    resolution.target_pts_us = 3000000;
    resolution.effective_duration_us = 3000000;
    resolution.clamped = true;

    const auto clamp = build_seek_clamp_log_facts(resolution);
    REQUIRE(clamp.should_log);
    REQUIRE(clamp.requested_seconds == 5.0);
    REQUIRE(clamp.clamped_seconds == 3.0);
    REQUIRE(clamp.duration_seconds == 3.0);

    resolution.clamped = false;
    REQUIRE_FALSE(build_seek_clamp_log_facts(resolution).should_log);
}

TEST_CASE("RendererSeekLogPolicy: builds per-track seek log facts",
          "[renderer][seek][log]") {
    TrackSeekTargetResolution target;
    target.requested_target_us = 7000000;
    target.target_us = 6500000;
    target.clamped = true;

    auto track_clamp = build_track_seek_target_clamp_log_facts(2, target);
    REQUIRE(track_clamp.should_log);
    REQUIRE(track_clamp.slot == 2);
    REQUIRE(track_clamp.requested_seconds == 7.0);
    REQUIRE(track_clamp.clamped_seconds == 6.5);

    target.clamped = false;
    REQUIRE_FALSE(build_track_seek_target_clamp_log_facts(2, target).should_log);
    target.clamped = true;

    TrackSeekPreparationResult preparation;
    preparation.buffered_frames_before = 5;
    preparation.packet_queue_size_before = 99;
    preparation.buffer_state_before = TrackState::Buffering;

    TrackSeekExecutionResult execution;
    execution.coalescing_transition = true;
    execution.applied_seek = false;

    const auto coalescing = build_track_seek_coalescing_log_facts(
        1, target, preparation, execution);
    REQUIRE(coalescing.should_log);
    REQUIRE(coalescing.slot == 1);
    REQUIRE(coalescing.buffer_state_before == static_cast<int>(TrackState::Buffering));
    REQUIRE(coalescing.target_seconds == 6.5);

    execution.coalescing_transition = false;
    REQUIRE_FALSE(build_track_seek_coalescing_log_facts(
        1, target, preparation, execution).should_log);

    execution.applied_seek = true;
    const auto cleared = build_track_seek_cleared_log_facts(
        1, target, preparation, execution, 0);
    REQUIRE(cleared.should_log);
    REQUIRE(cleared.slot == 1);
    REQUIRE(cleared.buffered_frames_before == 5);
    REQUIRE(cleared.buffered_frames_after == 0);
    REQUIRE(cleared.packet_queue_size_before == 99);
    REQUIRE(cleared.target_seconds == 6.5);

    execution.applied_seek = false;
    REQUIRE_FALSE(build_track_seek_cleared_log_facts(
        1, target, preparation, execution, 0).should_log);
}
