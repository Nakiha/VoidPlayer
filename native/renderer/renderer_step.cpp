#include "renderer/renderer_internal.h"

namespace vr {

void Renderer::Impl::step_forward() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    PresentDecision step_decision;
    bool have_step_decision = false;
    bool need_decode_wait = false;
    bool need_exact_seek = false;
    int64_t exact_seek_target = 0;
    int64_t exact_seek_decode_target = 0;
    StepDecisionApplication step_application;
    PresentDecision conservative_step_decision;
    bool have_conservative_step_decision = false;
    const auto build_step_decision_locked = [this](PresentDecision& decision) {
        return track_controller_.build_step_forward_decision(
            timeline_.playback().clock().current_pts_us(),
            present_history_.snapshot(),
            decision);
    };
    const auto apply_step_decision_locked =
        [this](const PresentDecision& decision) {
            return track_controller_.apply_step_forward_decision(
                timeline_.playback().clock().current_pts_us(),
                decision,
                present_history_.snapshot());
    };
    const auto set_video_decode_paused_locked = [this](bool paused) {
        track_controller_.set_video_decode_paused(
            paused,
            [](size_t, TrackPipeline& track, bool paused) {
                track.decode_thread->set_decode_paused(paused);
            });
    };

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto step_plan = plan_renderer_step_command(
            initialized_,
            initialized_ && track_controller_.has_buffering_track());
        if (!step_plan.execute) return;

        if (step_plan.pause_clock) {
            timeline_.playback().clock().pause();
        }
        timeline_.set_playing(step_plan.playing);

        const auto step_result = build_step_decision_locked(step_decision);
        if (step_result.has_decision && !step_result.needs_lookahead) {
            step_application = apply_step_decision_locked(step_decision);
            if (step_application.has_clock_target) {
                timeline_.playback().clock().seek(step_application.clock_target_us);
            }
            have_step_decision = true;
        } else {
            if (step_result.has_decision) {
                conservative_step_decision = step_decision;
                have_conservative_step_decision = true;
            }
            track_controller_.discard_step_forward_consumed_frames(
                timeline_.playback().clock().current_pts_us(),
                present_history_.snapshot(),
                present_history_.snapshot());
            set_video_decode_paused_locked(false);
            need_decode_wait = true;
        }
    }

    if (need_decode_wait) {
        const auto deadline = std::chrono::steady_clock::now() + kStepForwardDecodeWait;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!initialized_) return;
                const auto step_result = build_step_decision_locked(step_decision);
                if (step_result.has_decision && !step_result.needs_lookahead) {
                    set_video_decode_paused_locked(true);
                    step_application = apply_step_decision_locked(step_decision);
                    if (step_application.has_clock_target) {
                        timeline_.playback().clock().seek(step_application.clock_target_us);
                    }
                    have_step_decision = true;
                    break;
                }
                if (step_result.has_decision) {
                    conservative_step_decision = step_decision;
                    have_conservative_step_decision = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        if (!have_step_decision) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!initialized_) return;
            set_video_decode_paused_locked(true);
            if (have_conservative_step_decision) {
                step_decision = conservative_step_decision;
                step_application = apply_step_decision_locked(step_decision);
                if (step_application.has_clock_target) {
                    timeline_.playback().clock().seek(step_application.clock_target_us);
                }
                have_step_decision = true;
            }
        }

        if (!have_step_decision) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!initialized_) return;
            const auto clock_pts_us =
                timeline_.playback().clock().current_pts_us();
            std::optional<int64_t> logical_step_anchor_us;
            if (step_forward_exact_seek_anchor_us_ == clock_pts_us) {
                logical_step_anchor_us = clock_pts_us;
            }
            const auto fallback_seek =
                track_controller_.choose_step_forward_exact_seek_target(
                    clock_pts_us,
                    present_history_.snapshot(),
                    logical_step_anchor_us);
            exact_seek_target = fallback_seek.target_pts_us;
            exact_seek_decode_target = fallback_seek.decode_target_pts_us;
            spdlog::info("[Renderer] step_forward exact_seek: visible_pts={:.3f}s, clock_pts={:.3f}s, duration={:.3f}ms, target={:.3f}s, decode_target={:.3f}s",
                         fallback_seek.has_visible_pts
                             ? fallback_seek.visible_pts_us / 1e6
                             : -1.0,
                         fallback_seek.clock_pts_us / 1e6,
                         fallback_seek.frame_duration_us / 1e3,
                         exact_seek_target / 1e6,
                         exact_seek_decode_target / 1e6);
            need_exact_seek = true;
        }
    }

    if (have_step_decision) {
        auto present_context = present_command_context();
        RendererPresentCommandProcessor::present_frame(
            present_context, step_decision);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            present_history_.set(step_decision);
            step_forward_exact_seek_anchor_us_.reset();
        }
        double pts = step_application.has_clock_target
                     ? step_application.presented_pts_us / 1e6 : -1.0;
        spdlog::info("[Renderer] draw_paused_frame(step_forward): pts={:.3f}s", pts);
        return;
    }

    if (need_exact_seek) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        SeekCommandProcessor::seek(
            *this,
            lock,
            exact_seek_decode_target,
            SeekType::ExactStepForward,
            false);
        timeline_.playback().clock().seek(exact_seek_target);
        step_forward_exact_seek_anchor_us_ = exact_seek_target;
        spdlog::info("[Renderer] step_forward exact_seek done: clock_pts={:.3f}s",
                     timeline_.playback().clock().current_pts_us() / 1e6);
    }
}

void Renderer::Impl::step_backward() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    PresentDecision step_decision;
    bool have_step_decision = false;
    StepDecisionApplication step_application;
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        const auto step_plan = plan_renderer_step_command(
            initialized_,
            initialized_ && track_controller_.has_buffering_track());
        if (!step_plan.execute) return;
        step_forward_exact_seek_anchor_us_.reset();

        if (step_plan.pause_clock) {
            timeline_.playback().clock().pause();
        }
        timeline_.set_playing(step_plan.playing);

        if (track_controller_.build_step_backward_decision(
                timeline_.playback().clock().current_pts_us(),
                present_history_.snapshot(),
                step_decision)) {
            step_application = track_controller_.apply_step_backward_decision(
                step_decision);
            if (step_application.has_clock_target) {
                timeline_.playback().clock().seek(step_application.clock_target_us);
            }
            have_step_decision = true;
        } else {
            // Cache miss: exact seek to (current_pts - frame_duration - margin)
            // Add 1ms margin: frame duration is integer-truncated (e.g. 1/60s → 16666us)
            // but actual PTS spacing is 16667us, so (pts - dur) overshoots the
            // previous frame by 1us and exact seek's "< target" check discards it.
            const auto fallback_seek =
                track_controller_.choose_step_backward_exact_seek_target(
                    timeline_.playback().clock().current_pts_us(),
                    present_history_.snapshot());
            const int64_t target = fallback_seek.target_pts_us;
            spdlog::info("[Renderer] step_backward exact_seek: visible_pts={:.3f}s, clock_pts={:.3f}s, duration={:.3f}ms, target={:.3f}s",
                         fallback_seek.base_pts_us / 1e6,
                         fallback_seek.clock_pts_us / 1e6,
                         fallback_seek.frame_duration_us / 1e3,
                         target / 1e6);
            SeekCommandProcessor::seek(
                *this, lock, target, SeekType::Exact);
            spdlog::info("[Renderer] step_backward exact_seek done: clock_pts={:.3f}s",
                         timeline_.playback().clock().current_pts_us() / 1e6);
            // Don't draw stale frame; seek_internal requested a preview redraw,
            // render loop will draw the new frame when decode completes.
            return;
        }
    }
    if (have_step_decision) {
        auto present_context = present_command_context();
        RendererPresentCommandProcessor::present_frame(
            present_context, step_decision);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            present_history_.set(step_decision);
        }
        double pts = step_application.has_clock_target
                     ? step_application.presented_pts_us / 1e6 : -1.0;
        spdlog::info("[Renderer] draw_paused_frame(step_backward): pts={:.3f}s", pts);
    }
}

} // namespace vr
