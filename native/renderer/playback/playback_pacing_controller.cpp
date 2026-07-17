#include "renderer/playback/playback_pacing_controller.h"

#include <algorithm>
#include <cmath>

namespace vr {
namespace {

constexpr double kMinimumPacingScale = 0.25;
constexpr double kSpeedUpdateEpsilon = 0.01;

} // namespace

double PlaybackPacingController::choose_effective_speed(
    double requested_speed,
    bool allow_adaptive_speed,
    const PlaybackPacingSnapshot& snapshot) {
    if (requested_speed <= 0.0 ||
        !allow_adaptive_speed ||
        !snapshot.frontier_limited ||
        snapshot.bottleneck_target_frames <= 1) {
        return std::max(requested_speed, 0.0);
    }

    const size_t buffered_ahead =
        snapshot.bottleneck_buffered_frames > 0
            ? snapshot.bottleneck_buffered_frames - 1
            : 0;
    const size_t target_ahead =
        snapshot.bottleneck_target_frames - 1;
    const double fill = std::clamp(
        static_cast<double>(buffered_ahead) /
            static_cast<double>(target_ahead),
        0.0,
        1.0);
    const double pacing_scale =
        kMinimumPacingScale + (1.0 - kMinimumPacingScale) * fill;
    return requested_speed * pacing_scale;
}

bool PlaybackPacingController::update_effective_speed(
    double speed,
    PlaybackPacingDecision& decision) {
    if (std::abs(speed - effective_speed_) < kSpeedUpdateEpsilon) {
        decision.effective_speed = effective_speed_;
        return false;
    }
    effective_speed_ = speed;
    decision.effective_speed = speed;
    decision.update_effective_speed = true;
    return true;
}

PlaybackPacingDecision PlaybackPacingController::evaluate(
    const PlaybackPacingInput& input) {
    PlaybackPacingDecision decision;
    decision.state = state_;
    decision.effective_speed = effective_speed_;
    diagnostics_.requested_speed = input.requested_speed;
    diagnostics_.bottleneck_slot = input.snapshot.bottleneck_slot;
    diagnostics_.min_buffered_frames = input.snapshot.min_buffered_frames;
    diagnostics_.bottleneck_buffered_frames =
        input.snapshot.bottleneck_buffered_frames;
    diagnostics_.bottleneck_target_frames =
        input.snapshot.bottleneck_target_frames;
    diagnostics_.safe_frontier_us = input.snapshot.safe_frontier_us;
    diagnostics_.headroom_us = input.snapshot.headroom_us;

    if (!input.playing || !input.snapshot.has_active_tracks) {
        if (!input.pacing_held) {
            state_ = PlaybackPacingState::Idle;
        }
        decision.state = state_;
        update_effective_speed(input.requested_speed, decision);
        diagnostics_.state = state_;
        diagnostics_.effective_speed = effective_speed_;
        return decision;
    }

    if (input.snapshot.preroll_blocked) {
        decision.entered_preroll =
            state_ != PlaybackPacingState::Preroll;
        state_ = PlaybackPacingState::Preroll;
        decision.state = state_;
        decision.hold_for_pacing = !input.pacing_held;
        diagnostics_.state = state_;
        diagnostics_.effective_speed = effective_speed_;
        return decision;
    }

    if (state_ == PlaybackPacingState::Idle && input.pacing_held) {
        // A seek or lifecycle reset may replace controller history while the
        // PlaybackController still owns a valid pacing hold. Recover through
        // the normal hysteresis path instead of leaving that hold orphaned.
        state_ = PlaybackPacingState::Rebuffering;
    }

    if (state_ == PlaybackPacingState::Preroll ||
        state_ == PlaybackPacingState::Rebuffering) {
        if (!input.snapshot.resume_ready) {
            decision.hold_for_pacing = !input.pacing_held;
            decision.state = state_;
            diagnostics_.state = state_;
            diagnostics_.effective_speed = effective_speed_;
            return decision;
        }

        const PlaybackPacingState recovered_from = state_;
        const double resumed_speed =
            choose_effective_speed(
                input.requested_speed,
                input.allow_adaptive_speed,
                input.snapshot);
        update_effective_speed(resumed_speed, decision);
        decision.release_pacing_hold = input.pacing_held;
        decision.resume_decode = true;
        decision.resumed_running = true;
        state_ = PlaybackPacingState::Running;
        decision.state = state_;
        if (recovered_from == PlaybackPacingState::Rebuffering &&
            rebuffer_started_us_ > 0 &&
            input.monotonic_time_us >= rebuffer_started_us_) {
            diagnostics_.rebuffer_duration_us +=
                static_cast<uint64_t>(
                    input.monotonic_time_us - rebuffer_started_us_);
        }
        rebuffer_started_us_ = 0;
        diagnostics_.state = state_;
        diagnostics_.effective_speed = effective_speed_;
        return decision;
    }

    if (input.snapshot.starvation_risk &&
        !input.snapshot.all_active_tracks_eof) {
        state_ = PlaybackPacingState::Rebuffering;
        decision.state = state_;
        decision.entered_rebuffering = true;
        decision.hold_for_pacing = !input.pacing_held;
        ++diagnostics_.rebuffer_count;
        rebuffer_started_us_ = input.monotonic_time_us;
        diagnostics_.state = state_;
        diagnostics_.effective_speed = effective_speed_;
        return decision;
    }

    state_ = PlaybackPacingState::Running;
    decision.state = state_;
    update_effective_speed(
        choose_effective_speed(
            input.requested_speed,
            input.allow_adaptive_speed,
            input.snapshot),
        decision);
    diagnostics_.state = state_;
    diagnostics_.effective_speed = effective_speed_;
    return decision;
}

void PlaybackPacingController::reset() {
    state_ = PlaybackPacingState::Idle;
    effective_speed_ = 1.0;
    rebuffer_started_us_ = 0;
    diagnostics_ = {};
}

const char* playback_pacing_state_name(PlaybackPacingState state) {
    switch (state) {
    case PlaybackPacingState::Idle:
        return "idle";
    case PlaybackPacingState::Preroll:
        return "preroll";
    case PlaybackPacingState::Running:
        return "running";
    case PlaybackPacingState::Rebuffering:
        return "rebuffering";
    }
    return "unknown";
}

} // namespace vr
