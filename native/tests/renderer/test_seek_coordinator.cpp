#include <catch2/catch_test_macros.hpp>

#include "renderer/seek/seek_coordinator.h"

#include <chrono>
#include <thread>

using namespace vr;

TEST_CASE("SeekTargetPolicy: clamps requested target to playable range",
          "[seek][coordinator][target]") {
    PendingSeekPreviewEventState no_pending;

    auto result = resolve_seek_target(5000, 10000, no_pending);
    REQUIRE(result.requested_pts_us == 5000);
    REQUIRE(result.target_pts_us == 5000);
    REQUIRE(result.effective_duration_us == 10000);
    REQUIRE_FALSE(result.clamped);
    REQUIRE_FALSE(result.retarget_pending_event);

    result = resolve_seek_target(-250, 10000, no_pending);
    REQUIRE(result.target_pts_us == 0);
    REQUIRE(result.clamped);

    result = resolve_seek_target(12000, 10000, no_pending);
    REQUIRE(result.target_pts_us == 10000);
    REQUIRE(result.clamped);

    result = resolve_seek_target(-250, 0, no_pending);
    REQUIRE(result.target_pts_us == 0);
    REQUIRE(result.clamped);

    result = resolve_seek_target(12000, 0, no_pending);
    REQUIRE(result.target_pts_us == 12000);
    REQUIRE_FALSE(result.clamped);
}

TEST_CASE("SeekTargetPolicy: retargets matching pending preview event only when clamped",
          "[seek][coordinator][target]") {
    PendingSeekPreviewEventState pending;
    pending.has_request = true;
    pending.emitted = false;
    pending.target_pts_us = 12000;

    auto result = resolve_seek_target(12000, 10000, pending);
    REQUIRE(result.target_pts_us == 10000);
    REQUIRE(result.clamped);
    REQUIRE(result.retarget_pending_event);

    pending.target_pts_us = 11000;
    result = resolve_seek_target(12000, 10000, pending);
    REQUIRE_FALSE(result.retarget_pending_event);

    pending.target_pts_us = 12000;
    pending.emitted = true;
    result = resolve_seek_target(12000, 10000, pending);
    REQUIRE_FALSE(result.retarget_pending_event);

    pending.emitted = false;
    pending.has_request = false;
    result = resolve_seek_target(12000, 10000, pending);
    REQUIRE_FALSE(result.retarget_pending_event);

    pending.has_request = true;
    result = resolve_seek_target(9000, 10000, pending);
    REQUIRE_FALSE(result.clamped);
    REQUIRE_FALSE(result.retarget_pending_event);
}

TEST_CASE("RendererSeekClockGatePolicy: always advances clock to resolved target",
          "[seek][coordinator][clock]") {
    RendererSeekClockGateInput input;
    input.allow_deferred = true;
    input.playing = false;
    input.has_hevc_hw_track = true;
    input.target_pts_us = 12345;
    input.type = SeekType::Exact;

    const auto plan = plan_renderer_seek_clock_gate(input);
    REQUIRE(plan.seek_clock);
    REQUIRE(plan.target_pts_us == 12345);
    REQUIRE(plan.type == SeekType::Exact);
    REQUIRE_FALSE(plan.playing);
    REQUIRE(plan.has_hevc_hw_track);
}

TEST_CASE("RendererSeekClockGatePolicy: only paused HEVC exact seeks enter defer gate",
          "[seek][coordinator][clock]") {
    RendererSeekClockGateInput input;
    input.allow_deferred = true;
    input.playing = false;
    input.has_hevc_hw_track = true;
    input.target_pts_us = 1000;
    input.type = SeekType::Exact;

    auto plan = plan_renderer_seek_clock_gate(input);
    REQUIRE(plan.evaluate_paused_hevc_defer);

    input.playing = true;
    plan = plan_renderer_seek_clock_gate(input);
    REQUIRE_FALSE(plan.evaluate_paused_hevc_defer);

    input.playing = false;
    input.has_hevc_hw_track = false;
    plan = plan_renderer_seek_clock_gate(input);
    REQUIRE_FALSE(plan.evaluate_paused_hevc_defer);

    input.has_hevc_hw_track = true;
    input.type = SeekType::Keyframe;
    plan = plan_renderer_seek_clock_gate(input);
    REQUIRE_FALSE(plan.evaluate_paused_hevc_defer);

    input.type = SeekType::Exact;
    input.allow_deferred = false;
    plan = plan_renderer_seek_clock_gate(input);
    REQUIRE_FALSE(plan.evaluate_paused_hevc_defer);
}

TEST_CASE("HevcSeekRecreatePolicy: ignores non-HEVC hardware seeks",
          "[seek][coordinator][hevc]") {
    HevcSeekRecreateInput input;
    input.is_hevc_hw_seek = false;
    input.paused_seek = false;
    input.seek_transition_active = false;
    input.seek_type = SeekType::Keyframe;

    const auto decision = choose_hevc_seek_recreate(input);
    REQUIRE_FALSE(decision.should_recreate_pipeline);
    REQUIRE_FALSE(decision.error_if_recreate_not_applied);
    REQUIRE_FALSE(decision.coalescing_transition);
}

TEST_CASE("HevcSeekRecreatePolicy: playing seek recreates or errors",
          "[seek][coordinator][hevc]") {
    HevcSeekRecreateInput input;
    input.is_hevc_hw_seek = true;
    input.paused_seek = false;
    input.seek_transition_active = false;
    input.seek_type = SeekType::Exact;

    const auto decision = choose_hevc_seek_recreate(input);
    REQUIRE(decision.should_recreate_pipeline);
    REQUIRE(decision.error_if_recreate_not_applied);
    REQUIRE_FALSE(decision.coalescing_transition);
}

TEST_CASE("HevcSeekRecreatePolicy: transition coalesces without playing error",
          "[seek][coordinator][hevc]") {
    HevcSeekRecreateInput input;
    input.is_hevc_hw_seek = true;
    input.paused_seek = false;
    input.seek_transition_active = true;
    input.seek_type = SeekType::Exact;

    const auto decision = choose_hevc_seek_recreate(input);
    REQUIRE_FALSE(decision.should_recreate_pipeline);
    REQUIRE_FALSE(decision.error_if_recreate_not_applied);
    REQUIRE(decision.coalescing_transition);
}

TEST_CASE("HevcSeekRecreatePolicy: paused keyframe recreate is one-shot",
          "[seek][coordinator][hevc]") {
    HevcSeekRecreateInput input;
    input.is_hevc_hw_seek = true;
    input.paused_seek = true;
    input.seek_transition_active = false;
    input.seek_type = SeekType::Keyframe;

    auto decision = choose_hevc_seek_recreate(input);
    REQUIRE(decision.should_recreate_pipeline);
    REQUIRE_FALSE(decision.error_if_recreate_not_applied);
    REQUIRE_FALSE(decision.coalescing_transition);

    input.recreated_for_paused_hevc_seek = true;
    decision = choose_hevc_seek_recreate(input);
    REQUIRE_FALSE(decision.should_recreate_pipeline);
    REQUIRE_FALSE(decision.error_if_recreate_not_applied);
    REQUIRE_FALSE(decision.coalescing_transition);
}

TEST_CASE("HevcSeekRecreatePolicy: paused exact seek does not recreate",
          "[seek][coordinator][hevc]") {
    HevcSeekRecreateInput input;
    input.is_hevc_hw_seek = true;
    input.paused_seek = true;
    input.seek_transition_active = false;
    input.seek_type = SeekType::Exact;

    const auto decision = choose_hevc_seek_recreate(input);
    REQUIRE_FALSE(decision.should_recreate_pipeline);
    REQUIRE_FALSE(decision.error_if_recreate_not_applied);
    REQUIRE_FALSE(decision.coalescing_transition);
}

TEST_CASE("HevcSeekRecreatePolicy: forced paused transition can recreate",
          "[seek][coordinator][hevc]") {
    HevcSeekRecreateInput input;
    input.is_hevc_hw_seek = true;
    input.paused_seek = true;
    input.seek_transition_active = true;
    input.force_recreate_paused_hevc = true;
    input.seek_type = SeekType::Keyframe;

    const auto decision = choose_hevc_seek_recreate(input);
    REQUIRE(decision.should_recreate_pipeline);
    REQUIRE_FALSE(decision.error_if_recreate_not_applied);
    REQUIRE(decision.coalescing_transition);
}

TEST_CASE("LoopRangeSeekPolicy: seeks to loop start at or past end",
          "[seek][coordinator][loop]") {
    LoopRangeSeekInput input;
    input.playing = true;
    input.loop_enabled = true;
    input.clock_paused = false;
    input.start_us = 1000;
    input.end_us = 4000;

    input.current_pts_us = 3999;
    auto decision = choose_loop_range_seek(input);
    REQUIRE_FALSE(decision.should_seek);

    input.current_pts_us = 4000;
    decision = choose_loop_range_seek(input);
    REQUIRE(decision.should_seek);
    REQUIRE(decision.target_pts_us == 1000);

    input.current_pts_us = 4500;
    decision = choose_loop_range_seek(input);
    REQUIRE(decision.should_seek);
    REQUIRE(decision.target_pts_us == 1000);
}

TEST_CASE("LoopRangeState: normalizes and compares loop ranges",
          "[seek][coordinator][loop]") {
    auto state = normalize_loop_range_state(true, 1000, 4000);
    REQUIRE(state.enabled);
    REQUIRE(state.start_us == 1000);
    REQUIRE(state.end_us == 4000);

    state = normalize_loop_range_state(false, 1000, 4000);
    REQUIRE_FALSE(state.enabled);
    REQUIRE(state.start_us == 0);
    REQUIRE(state.end_us == 0);

    state = normalize_loop_range_state(true, 4000, 1000);
    REQUIRE_FALSE(state.enabled);

    auto same = normalize_loop_range_state(true, 1000, 4000);
    auto different = normalize_loop_range_state(true, 1000, 5000);
    REQUIRE(loop_range_states_equal(same, normalize_loop_range_state(true, 1000, 4000)));
    REQUIRE_FALSE(loop_range_states_equal(same, different));
}

TEST_CASE("LoopRangeSeekPolicy: ignores inactive or invalid loop states",
          "[seek][coordinator][loop]") {
    LoopRangeSeekInput input;
    input.playing = true;
    input.loop_enabled = true;
    input.clock_paused = false;
    input.current_pts_us = 5000;
    input.start_us = 1000;
    input.end_us = 4000;

    auto decision = choose_loop_range_seek(input);
    REQUIRE(decision.should_seek);

    input.playing = false;
    decision = choose_loop_range_seek(input);
    REQUIRE_FALSE(decision.should_seek);

    input.playing = true;
    input.loop_enabled = false;
    decision = choose_loop_range_seek(input);
    REQUIRE_FALSE(decision.should_seek);

    input.loop_enabled = true;
    input.clock_paused = true;
    decision = choose_loop_range_seek(input);
    REQUIRE_FALSE(decision.should_seek);

    input.clock_paused = false;
    input.end_us = input.start_us;
    decision = choose_loop_range_seek(input);
    REQUIRE_FALSE(decision.should_seek);
}

TEST_CASE("SeekCoordinator: only paused exact HEVC hardware seek enters coordinator",
          "[seek][coordinator]") {
    SeekCoordinator coordinator(std::chrono::milliseconds(1));

    REQUIRE_FALSE(coordinator.should_defer_paused_hevc_seek(
        true, true, 1000, SeekType::Exact));
    REQUIRE_FALSE(coordinator.should_defer_paused_hevc_seek(
        false, false, 1000, SeekType::Exact));
    REQUIRE_FALSE(coordinator.should_defer_paused_hevc_seek(
        false, true, 1000, SeekType::Keyframe));
    REQUIRE_FALSE(coordinator.paused_hevc_seek_in_flight());
}

TEST_CASE("SeekCoordinator: serializes paused HEVC exact seeks during settle window",
          "[seek][coordinator]") {
    SeekCoordinator coordinator(std::chrono::milliseconds(1));

    REQUIRE_FALSE(coordinator.should_defer_paused_hevc_seek(
        false, true, 1000, SeekType::Exact));
    REQUIRE(coordinator.paused_hevc_seek_in_flight());

    REQUIRE(coordinator.should_defer_paused_hevc_seek(
        false, true, 2000, SeekType::Exact));
    REQUIRE(coordinator.has_pending_deferred_seek());

    coordinator.mark_paused_hevc_preview_drawn(true);
    REQUIRE_FALSE(coordinator.paused_hevc_seek_in_flight());
    REQUIRE_FALSE(coordinator.take_deferred_paused_hevc_seek(false).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto deferred = coordinator.take_deferred_paused_hevc_seek(false);
    REQUIRE(deferred.has_value());
    REQUIRE(deferred->target_pts_us == 2000);
    REQUIRE(deferred->type == SeekType::Exact);
    REQUIRE(coordinator.paused_hevc_seek_in_flight());
}

TEST_CASE("SeekCoordinator: reset clears deferred state", "[seek][coordinator]") {
    SeekCoordinator coordinator(std::chrono::milliseconds(1));

    REQUIRE_FALSE(coordinator.should_defer_paused_hevc_seek(
        false, true, 1000, SeekType::Exact));
    REQUIRE(coordinator.should_defer_paused_hevc_seek(
        false, true, 2000, SeekType::Exact));
    coordinator.reset();

    REQUIRE_FALSE(coordinator.paused_hevc_seek_in_flight());
    REQUIRE_FALSE(coordinator.has_pending_deferred_seek());
}
