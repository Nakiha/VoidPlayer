#include "renderer/renderer_internal.h"

#ifdef _WIN32
#include "windows/d3d11/render_backend.h"
#endif

namespace vr {

void Renderer::Impl::do_resize(int width, int height) {
#ifdef _WIN32
    int old_width = 0;
    int old_height = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (width == surface_state_.width() &&
            height == surface_state_.height()) {
            return;
        }
        old_width = surface_state_.width();
        old_height = surface_state_.height();
    }

    spdlog::info("[Renderer] resize: {}x{} -> {}x{}", old_width, old_height, width, height);

    if (!presentation_.resize_renderer_managed_headless_output(
            width, height, presentation_metrics_)) {
        return;
    }

    RendererDrawSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto resize = surface_state_.resize_if_changed(width, height);
        old_width = resize.old_width;
        old_height = resize.old_height;
        const auto layout_tracks = track_controller_.layout_track_geometry();
        layout_state_.adjust_view_offset_for_resize(
            old_width, old_height, width, height, layout_tracks);
        snapshot = build_draw_snapshot_locked(present_history_.snapshot());
    }

    RendererFrameCallback frame_callback;
    bool drew = false;
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(presentation_.device_mutex());
        drew = presentation_.draw_renderer_managed_headless_and_publish(
            snapshot,
            "resize",
            presentation_metrics_,
            presentation_overlay_hooks(),
            [this]() { return shutting_down_.load(std::memory_order_acquire); },
            frame_callback);
    }
    if (drew) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (layout_state_.mark_presented_if_newer(layout_state_.current_revision())) {
            presentation_metrics_.note_layout_presented();
        }
        loop_driver_.mark_preview_presented(true);
    }
    if (frame_callback && !shutting_down_.load(std::memory_order_acquire)) {
        frame_callback(nullptr);
    }
#else
    (void)width;
    (void)height;
#endif
}

void Renderer::Impl::render_loop() noexcept {
    // On Windows this raises timer resolution from the default ~15.6 ms to
    // 1 ms; on other platforms it is a no-op wrapper.
    ScopedRenderThreadTiming render_thread_timing;
    spdlog::info("[Renderer] Render loop started (timer resolution: platform), tid={}",
                 current_render_thread_id_string());

    try {
        render_loop_body();
    } catch (const std::exception& e) {
        spdlog::error("[Renderer] Render loop crashed: {}", e.what());
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_render_loop_error_locked("std::exception");
    } catch (...) {
        spdlog::error("[Renderer] Render loop crashed with an unknown exception");
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_render_loop_error_locked("unknown exception");
    }

    loop_driver_.clear_pending_resize();
    spdlog::info("[Renderer] Render loop ended");
}

void Renderer::Impl::render_loop_body() {
    loop_driver_.start_loop(std::chrono::steady_clock::now());

    while (loop_driver_.running()) {
        if (presentation_.poll_device_removed("render_loop")) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            enter_terminal_device_lost_locked("render_loop");
            break;
        }

        {
            std::unique_lock<std::mutex> lifecycle_lock(
                lifecycle_mutex_, std::try_to_lock);
            if (lifecycle_lock.owns_lock()) {
                std::unique_lock<std::mutex> lock(state_mutex_);
                if (apply_deferred_paused_hevc_seek_locked(lock)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (apply_loop_range_locked(lock)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }
        }

        // Process pending resize (debounced — at most ~30Hz).
        {
            const auto resize =
                loop_driver_.take_resize_decision(std::chrono::steady_clock::now());
            if (resize.should_apply) {
                do_resize(resize.width, resize.height);
            }
        }

#ifdef _WIN32
        presentation_.cleanup_renderer_managed_headless_pending_buffers();
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
                set_decode_paused_for_all_tracks(false);
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
            // While paused/prerolling, draw current frame if not yet drawn
            bool should_draw_preview = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                consume_pending_layout_locked();
                should_draw_preview = loop_driver_.begin_preview_draw_if_needed();
            }
            if (should_draw_preview) {
                bool drawn = false;

                // Try cached last frame first (for layout changes while paused)
                RendererPausedCachedDecision cached;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    cached = track_controller_.paused_cached_decision(
                        present_history_.snapshot());
                }
                if (cached.has_frame) {
                    present_frame(cached.decision);
                    drawn = true;
                    spdlog::debug("[Renderer] Paused frame (cached): pts={:.3f}s",
                                  cached.first_pts_us.has_value()
                                      ? static_cast<double>(*cached.first_pts_us) / 1e6
                                      : -1.0);
                }

                // No cached frame — try track buffer (initial preview)
                // Only draw when ALL active tracks have frames, to avoid
                // flashing black for tracks that haven't finished seeking.
                if (!drawn) {
                    PausedPreviewSnapshot snapshot;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        snapshot = track_controller_.paused_preview_snapshot();
                    }
                    if (snapshot.ready_to_present) {
                        auto& preview = snapshot.decision;
                        present_frame(preview);
                        drawn = true;
                        bool preserve_requested_clock = false;
                        RendererTrackReferenceSnapshot ref;
                        if (!playing_snapshot) {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            present_history_.set(preview);
                            set_decode_paused_for_all_tracks(true);
                            preserve_requested_clock = true;
                            mark_paused_hevc_seek_preview_drawn_locked();
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
                        emit_seek_preview_presented_events(preview);
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

        // Periodic diagnostics
        {
            auto now = std::chrono::steady_clock::now();
            int64_t pts = timeline_.playback().clock().current_pts_us();
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
                                 track.slot, pts / 1e6,
                                 diagnostic.pts_delta_us / 1e3,
                                 track.buffer_count, track.buffer_capacity,
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
            // so each track always shows a frame (new or carried over).
            // Once a track has started, keep carrying its last frame even after
            // that track reaches EOF. This lets shorter tracks freeze on their
            // final image while longer tracks continue playing.
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                track_controller_.apply_carry_forward(
                    present_history_.snapshot(), decision);
            }
            present_frame(decision);
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

        // Frame-driven clock: when buffer is empty, clamp clock to the
        // end of the last presented frame so PTS doesn't run ahead.
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
                if (settle_eof_locked(eof_clamp.max_end_pts_us)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }
        }

        // Deadline-based sleep: wake up at the exact wall time when the next
        // frame should be displayed.  This is drift-free because each sleep
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
                // No frames available (buffer underflow) — short poll
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    loop_driver_.clear_pending_resize();
}

RendererPresentationOverlayHooks Renderer::Impl::presentation_overlay_hooks() {
    RendererPresentationOverlayHooks hooks;
#ifdef _WIN32
    hooks.draw_overlay = [this](PresentationBackend& backend,
                                const RendererDrawSnapshot& draw_snapshot) {
        if (!analysis_overlay_renderer_ ||
            backend.kind() != PresentationBackendKind::D3D11) {
            return;
        }
        auto* d3d = static_cast<D3D11RenderBackend*>(&backend);
        auto* device = d3d->device();
        auto* resources = d3d->resources();
        if (!device || !resources) {
            return;
        }
        analysis_overlay_renderer_->draw(
            draw_snapshot,
            *device,
            *resources,
            draw_snapshot.target_width,
            draw_snapshot.target_height);
    };
#else
    hooks.draw_overlay = [](PresentationBackend& backend,
                            const RendererDrawSnapshot& draw_snapshot) {
        (void)backend;
        (void)draw_snapshot;
    };
#endif
    hooks.composite_bgra_overlay =
        [this](const RendererDrawSnapshot& draw_snapshot,
               uint8_t* target_bgra,
               int target_width,
               int target_height,
               size_t target_stride_bytes) {
            if (!analysis_overlay_renderer_) {
                return false;
            }
            return analysis_overlay_renderer_->composite_bgra(
                draw_snapshot,
                target_bgra,
                target_width,
                target_height,
                target_stride_bytes);
        };
    return hooks;
}

} // namespace vr
