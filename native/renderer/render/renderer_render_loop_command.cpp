#include "renderer/render/renderer_render_loop_command.h"

#include "renderer/render/renderer_timing_utils.h"
#include "renderer/render/renderer_viewport_trace.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

namespace vr {

void RendererRenderLoopCommandProcessor::run_body(
    RendererRenderLoopCommandContext& context) {
    auto& loop_driver_ = context.loop;
    auto& presentation_ = context.presentation;
    auto& state_mutex_ = context.state_mutex;
    auto& lifecycle_mutex_ = context.lifecycle_mutex;
    auto& timeline_ = context.timeline;
    auto& track_controller_ = context.tracks;
    auto& presentation_metrics_ = context.metrics;
    auto& render_sink_ = context.render_sink;
    auto& present_history_ = context.history;
    auto& layout_state_ = context.layout;
    loop_driver_.start_loop(std::chrono::steady_clock::now());

    while (loop_driver_.running()) {
        if (presentation_.poll_device_removed("render_loop")) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            context.hooks.enter_terminal_device_lost_locked("render_loop");
            break;
        }

        {
            std::unique_lock<std::mutex> lifecycle_lock(
                lifecycle_mutex_, std::try_to_lock);
            if (lifecycle_lock.owns_lock()) {
                std::unique_lock<std::mutex> lock(state_mutex_);
                if (context.hooks.apply_deferred_paused_hevc_seek_locked(lock)) {
                    lock.unlock();
                    context.hooks.emit_playback_clock_event(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (context.hooks.apply_loop_range_locked(lock)) {
                    lock.unlock();
                    context.hooks.emit_playback_clock_event(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }
        }

        // Process pending resize (debounced - at most ~30Hz).
        {
            const auto resize =
                loop_driver_.take_resize_decision(std::chrono::steady_clock::now());
            if (resize.should_apply) {
                context.hooks.do_resize(resize.width, resize.height);
            }
        }

#ifdef _WIN32
        presentation_.cleanup_renderer_managed_offscreen_pending_buffers();
#endif

        bool playing_snapshot;
        bool clock_paused_snapshot;
        RendererLoopPrerollDecision preroll_decision;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            playing_snapshot = timeline_.playing();
            const bool any_buffering =
                track_controller_.has_preroll_blocking_track();
            preroll_decision = loop_driver_.evaluate_preroll(
                playing_snapshot,
                timeline_.playback().clock().is_paused(),
                any_buffering,
                timeline_.playback().clock().current_pts_us());
            if (preroll_decision.force_preview_redraw) {
                loop_driver_.force_preview_redraw();
            }
            if (preroll_decision.pause_clock) {
                timeline_.playback().clock().pause();
            }
            if (preroll_decision.resume_decode) {
                context.hooks.set_decode_paused_for_all_tracks(false);
            }
            if (preroll_decision.resume_clock) {
                timeline_.playback().clock().resume();
            }
            clock_paused_snapshot = preroll_decision.clock_paused;
        }
        if (preroll_decision.log_transition_complete) {
            spdlog::info("[Renderer] Preroll transition complete, forcing preview redraw");
        }
        if (preroll_decision.log_preroll_pending) {
            spdlog::info("[Renderer] Preroll: clock PENDING, some track buffering, "
                         "(playing={})", playing_snapshot);
        }
        if (preroll_decision.log_preroll_complete) {
            spdlog::info("[Renderer] === Preroll COMPLETE: all tracks ready, clock resumed, "
                         "playing_={}, pts={:.3f}s)",
                         playing_snapshot,
                         preroll_decision.preroll_complete_pts_s);
        }

        if (!playing_snapshot || clock_paused_snapshot) {
            if (const auto backoff =
                    presentation_metrics_.transient_backpressure_remaining(
                        steady_clock_us_now());
                backoff.count() > 0) {
                std::this_thread::sleep_for(
                    std::min(backoff, std::chrono::microseconds(2000)));
                continue;
            }
            // While paused/prerolling, draw current frame if not yet drawn.
            bool should_draw_preview = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                context.hooks.consume_pending_layout_locked();
                should_draw_preview = loop_driver_.begin_preview_draw_if_needed();
            }
            if (should_draw_preview) {
                bool drawn = false;

                // Try cached last frame first (for layout changes while paused).
                RendererPausedCachedDecision cached;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    cached = track_controller_.paused_cached_decision(
                        present_history_.snapshot());
                }
                if (cached.has_frame) {
                    auto present_context = context.hooks.present_command_context();
                    RendererPresentCommandProcessor::present_frame(
                        present_context, cached.decision);
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        present_history_.set(cached.decision);
                    }
                    drawn = true;
                    spdlog::debug("[Renderer] Paused frame (cached): pts={:.3f}s",
                                  cached.first_pts_us.has_value()
                                      ? static_cast<double>(*cached.first_pts_us) / 1e6
                                      : -1.0);
                }

                // No cached frame: try track buffer (initial preview). Only draw
                // when all active tracks have frames, to avoid flashing black for
                // tracks that have not finished seeking.
                if (!drawn) {
                    PausedPreviewSnapshot snapshot;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        snapshot = track_controller_.paused_preview_snapshot();
                    }
                    if (snapshot.ready_to_present) {
                        auto& preview = snapshot.decision;
                        auto present_context = context.hooks.present_command_context();
                        RendererPresentCommandProcessor::present_frame(
                            present_context, preview);
                        drawn = true;
                        bool preserve_requested_clock = false;
                        RendererTrackReferenceSnapshot ref;
                        if (!playing_snapshot) {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            present_history_.set(preview);
                            context.hooks.set_decode_paused_for_all_tracks(true);
                            preserve_requested_clock = true;
                            context.hooks.mark_paused_hevc_seek_preview_drawn_locked();
                            ref = track_controller_.first_active_reference();
                        } else {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            present_history_.set(preview);
                            ref = track_controller_.first_active_reference();
                        }
                        // Keep the logical clock at the user's requested target
                        // while paused. The decoded preview can land on a
                        // nearest/tail frame for individual tracks, but the
                        // timeline should not visually snap backward.
                        if (!preserve_requested_clock &&
                            ref.slot >= 0 &&
                            preview.frames[static_cast<size_t>(ref.slot)].has_value()) {
                            timeline_.playback().clock().seek(
                                preview.frames[static_cast<size_t>(ref.slot)]->pts_us +
                                    ref.offset_us);
                        }
                        double preview_pts_s = -1.0;
                        if (ref.slot >= 0) {
                            const auto ref_slot = static_cast<size_t>(ref.slot);
                            if (preview.frames[ref_slot].has_value()) {
                                preview_pts_s = preview.frames[ref_slot]->pts_us / 1e6;
                            }
                        }
                        spdlog::info("[Renderer] Paused frame: pts={:.3f}s",
                                     preview_pts_s);
                        context.hooks.emit_seek_preview_presented_events(preview);
                    }
                }
                if (!drawn) {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    loop_driver_.clear_preview_pending();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto scheduler_tick = loop_driver_.tick_presentation(*render_sink_);
        auto decision = scheduler_tick.decision;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            track_controller_.filter_present_decision(decision);
        }

        // Periodic diagnostics.
        {
            auto now = std::chrono::steady_clock::now();
            int64_t pts = timeline_.playback().clock().current_pts_us();
            context.hooks.emit_playback_clock_event(false);
            const auto diagnostic =
                loop_driver_.take_diagnostic_decision(now, pts);
            if (diagnostic.should_emit) {
                std::vector<RenderLoopTrackDiagnosticSnapshot> diagnostics;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    diagnostics = track_controller_.render_loop_diagnostics();
                }
                for (const auto& track : diagnostics) {
                    spdlog::info("[diag] track[{}]: pts={:.3f}s delta={:.1f}ms "
                                 "buf={}/{} state={} playing={}",
                                 track.slot,
                                 pts / 1e6,
                                 diagnostic.pts_delta_us / 1e3,
                                 track.buffer_count,
                                 track.buffer_capacity,
                                 static_cast<int>(track.buffer_state),
                                 playing_snapshot);
                }
            }
        }

        if (decision.should_present && scheduler_tick.should_notify) {
            if (const auto backoff =
                    presentation_metrics_.transient_backpressure_remaining(
                        steady_clock_us_now());
                backoff.count() > 0) {
                std::this_thread::sleep_for(
                    std::min(backoff, std::chrono::microseconds(2000)));
                continue;
            }
            // Independent presentation: fill missing tracks from last decision
            // so each track always shows a frame (new or carried over). Once a
            // track has started, keep carrying its last frame even after that
            // track reaches EOF.
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                track_controller_.apply_carry_forward(
                    present_history_.snapshot(), decision);
            }
            if (!context.hooks.interaction_presentation_active()) {
                auto present_context = context.hooks.present_command_context();
                RendererPresentCommandProcessor::present_frame(
                    present_context, decision);
            }
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                present_history_.set(decision);
            }
        } else if (playing_snapshot) {
            uint64_t layout_revision = 0;
            uint64_t presented_revision = 0;
            uint64_t pending_layout_revision = 0;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                layout_revision = layout_state_.current_revision();
                presented_revision = layout_state_.last_presented_revision();
                pending_layout_revision = layout_state_.latest_requested_revision();
            }
            const uint64_t latest_layout_revision =
                std::max(layout_revision, pending_layout_revision);
            if (latest_layout_revision > presented_revision) {
                const uint64_t pending_layout =
                    presentation_metrics_.note_playing_layout_redraw_suppressed();
                if (viewport_trace_enabled() && pending_layout % 120 == 0) {
                    spdlog::info(
                        "[ViewportTrace] native source=viewport_composite_skip reason=deferred-to-playback "
                        "layout_rev={} pending_layout_rev={} presented_layout_rev={} suppressed={}",
                        layout_revision,
                        pending_layout_revision,
                        presented_revision,
                        pending_layout);
                }
            }
        }

        // Frame-driven clock: when buffer is empty, clamp clock to the end of
        // the last presented frame so PTS does not run ahead.
        {
            bool settled_eof = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                const auto eof_clamp =
                    track_controller_.empty_buffer_eof_clamp(
                        present_history_.snapshot());
                if (eof_clamp.all_active_buffers_empty &&
                    eof_clamp.max_end_pts_us > 0) {
                    int64_t current = timeline_.playback().clock().current_pts_us();
                    if (current > eof_clamp.max_end_pts_us) {
                        timeline_.playback().clock().seek(eof_clamp.max_end_pts_us);
                    }
                    if (context.hooks.settle_eof_locked(eof_clamp.max_end_pts_us)) {
                        settled_eof = true;
                    }
                }
            }
            if (settled_eof) {
                context.hooks.emit_playback_clock_event(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
        }

        // Deadline-based sleep: wake up at the exact wall time when the next
        // frame should be displayed. This is drift-free because each sleep
        // targets an absolute PTS rather than an accumulated relative duration.
        {
            int64_t current_pts = timeline_.playback().clock().current_pts_us();
            std::optional<int64_t> next_event_pts;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                next_event_pts = track_controller_.next_frame_event_pts_us(current_pts);
            }
            if (next_event_pts.has_value()) {
                double spd = timeline_.playback().clock().speed();
                const auto sleep_for = loop_driver_.frame_deadline_sleep(
                    current_pts, *next_event_pts, spd, MAX_SLEEP_US);
                if (sleep_for.count() > 0) {
                    std::this_thread::sleep_for(sleep_for);
                }
            } else {
                // No frames available (buffer underflow): short poll.
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    loop_driver_.clear_pending_resize();
}

} // namespace vr
