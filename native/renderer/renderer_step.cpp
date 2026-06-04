#include "renderer/renderer_internal.h"

namespace vr {

void Renderer::step_forward() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    PresentDecision step_decision;
    bool have_step_decision = false;
    bool need_decode_wait = false;
    bool need_exact_seek = false;
    int64_t exact_seek_target = 0;
    StepDecisionApplication step_application;
    PresentDecision conservative_step_decision;
    bool have_conservative_step_decision = false;
    const auto build_step_decision_locked = [this](PresentDecision& decision) {
        return build_step_forward_decision_result(
            tracks_,
            playback_->clock().current_pts_us(),
            compute_min_current_frame_duration_us(tracks_),
            last_decision_,
            decision);
    };
    const auto apply_step_decision_locked =
        [this](const PresentDecision& decision) {
            return apply_step_forward_decision(
                tracks_,
                playback_->clock().current_pts_us(),
                decision,
                last_decision_);
    };
    const auto set_video_decode_paused_locked = [this](bool paused) {
        apply_track_video_decode_pause_state(
            tracks_,
            paused,
            [](size_t, TrackPipeline& track, bool paused) {
                track.decode_thread->set_decode_paused(paused);
            });
    };

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto step_plan = plan_renderer_step_command(
            initialized_,
            initialized_ && has_buffering_track(tracks_));
        if (!step_plan.execute) return;

        if (step_plan.pause_clock) {
            playback_->clock().pause();
        }
        playing_ = step_plan.playing;

        const auto step_result = build_step_decision_locked(step_decision);
        if (step_result.has_decision && !step_result.needs_lookahead) {
            step_application = apply_step_decision_locked(step_decision);
            if (step_application.has_clock_target) {
                playback_->clock().seek(step_application.clock_target_us);
            }
            have_step_decision = true;
        } else {
            if (step_result.has_decision) {
                conservative_step_decision = step_decision;
                have_conservative_step_decision = true;
            }
            discard_step_forward_consumed_frames(
                tracks_,
                playback_->clock().current_pts_us(),
                last_decision_,
                last_decision_);
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
                        playback_->clock().seek(step_application.clock_target_us);
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
                    playback_->clock().seek(step_application.clock_target_us);
                }
                have_step_decision = true;
            }
        }

        if (!have_step_decision) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!initialized_) return;
            const auto fallback_seek = choose_step_forward_exact_seek_target(
                tracks_,
                playback_->clock().current_pts_us(),
                cached_duration_us_,
                last_decision_);
            exact_seek_target = fallback_seek.target_pts_us;
            spdlog::info("[Renderer] step_forward exact_seek: visible_pts={:.3f}s, clock_pts={:.3f}s, duration={:.3f}ms, target={:.3f}s",
                         fallback_seek.base_pts_us / 1e6,
                         fallback_seek.clock_pts_us / 1e6,
                         fallback_seek.frame_duration_us / 1e3,
                         exact_seek_target / 1e6);
            need_exact_seek = true;
        }
    }

    if (have_step_decision) {
        present_frame(step_decision);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_decision_ = step_decision;
        }
        double pts = step_application.has_clock_target
                     ? step_application.presented_pts_us / 1e6 : -1.0;
        spdlog::info("[Renderer] draw_paused_frame(step_forward): pts={:.3f}s", pts);
        return;
    }

    if (need_exact_seek) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        seek_internal(lock, exact_seek_target, SeekType::ExactStepForward);
        spdlog::info("[Renderer] step_forward exact_seek done: clock_pts={:.3f}s",
                     playback_->clock().current_pts_us() / 1e6);
    }
}

void Renderer::step_backward() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    PresentDecision step_decision;
    bool have_step_decision = false;
    StepDecisionApplication step_application;
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        const auto step_plan = plan_renderer_step_command(
            initialized_,
            initialized_ && has_buffering_track(tracks_));
        if (!step_plan.execute) return;

        if (step_plan.pause_clock) {
            playback_->clock().pause();
        }
        playing_ = step_plan.playing;

        if (build_step_backward_decision(
                tracks_,
                playback_->clock().current_pts_us(),
                last_decision_,
                step_decision)) {
            step_application = apply_step_backward_decision(
                tracks_, step_decision);
            if (step_application.has_clock_target) {
                playback_->clock().seek(step_application.clock_target_us);
            }
            have_step_decision = true;
        } else {
            // Cache miss: exact seek to (current_pts - frame_duration - margin)
            // Add 1ms margin: frame duration is integer-truncated (e.g. 1/60s → 16666us)
            // but actual PTS spacing is 16667us, so (pts - dur) overshoots the
            // previous frame by 1us and exact seek's "< target" check discards it.
            const auto fallback_seek = choose_step_backward_exact_seek_target(
                tracks_,
                playback_->clock().current_pts_us(),
                last_decision_);
            const int64_t target = fallback_seek.target_pts_us;
            spdlog::info("[Renderer] step_backward exact_seek: visible_pts={:.3f}s, clock_pts={:.3f}s, duration={:.3f}ms, target={:.3f}s",
                         fallback_seek.base_pts_us / 1e6,
                         fallback_seek.clock_pts_us / 1e6,
                         fallback_seek.frame_duration_us / 1e3,
                         target / 1e6);
            seek_internal(lock, target, SeekType::Exact);
            spdlog::info("[Renderer] step_backward exact_seek done: clock_pts={:.3f}s",
                         playback_->clock().current_pts_us() / 1e6);
            // Don't draw stale frame — seek_internal set preview_drawn_=false,
            // render loop will draw the new frame when decode completes.
            return;
        }
    }
    if (have_step_decision) {
        present_frame(step_decision);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_decision_ = step_decision;
        }
        double pts = step_application.has_clock_target
                     ? step_application.presented_pts_us / 1e6 : -1.0;
        spdlog::info("[Renderer] draw_paused_frame(step_backward): pts={:.3f}s", pts);
    }
}

} // namespace vr
