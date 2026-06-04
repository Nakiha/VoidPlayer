#include "renderer/renderer_internal.h"

namespace vr {

void Renderer::do_resize(int width, int height) {
#ifdef _WIN32
    int old_width = 0;
    int old_height = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (width == target_width_ && height == target_height_) return;
        old_width = target_width_;
        old_height = target_height_;
    }

    spdlog::info("[Renderer] resize: {}x{} -> {}x{}", old_width, old_height, width, height);

    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        std::lock_guard<std::mutex> tex_lock(texture_mutex());
        auto* output = headless_output();
        if (!output || !output->resize_locked(width, height)) {
            return;
        }
    }
    presentation_backend_metrics_.shared_texture_resize_count.fetch_add(
        1, std::memory_order_relaxed);

    RendererDrawSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto layout_tracks = snapshot_layout_track_geometry(tracks_);
        adjust_layout_view_offset_for_resize(
            layout_, old_width, old_height, width, height, layout_tracks);
        target_width_ = width;
        target_height_ = height;
        snapshot = build_draw_snapshot_locked(last_decision_);
    }

    RendererFrameCallback frame_callback;
    bool drew = false;
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        drew = draw_headless_and_publish(snapshot, "resize", frame_callback);
    }
    if (drew) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (layout_revision_ > last_presented_layout_revision_) {
            last_presented_layout_revision_ = layout_revision_;
            presentation_backend_metrics_.layout_presented_count.fetch_add(
                1, std::memory_order_relaxed);
        }
        preview_drawn_ = true;
    }
    if (frame_callback && !shutting_down_.load(std::memory_order_acquire)) {
        frame_callback(nullptr);
    }
#else
    (void)width;
    (void)height;
#endif
}

void Renderer::render_loop() noexcept {
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

    pending_width_.store(0, std::memory_order_release);
    pending_height_.store(0, std::memory_order_release);
    spdlog::info("[Renderer] Render loop ended");
}

void Renderer::render_loop_body() {
    render_loop_controller_.start(std::chrono::steady_clock::now());

    while (running_) {
        auto* backend = presentation_backend_.get();
        if (backend && backend->poll_device_removed("render_loop")) {
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
            int pw = pending_width_.exchange(0);
            int ph = pending_height_.exchange(0);
            if (pw > 0 && ph > 0) {
                auto now = std::chrono::steady_clock::now();
                if (render_loop_controller_.should_apply_resize(now)) {
                    do_resize(pw, ph);
                    render_loop_controller_.mark_resize_applied(now);
                } else {
                    // Too soon — re-queue so the next iteration can pick it up.
                    // Write back only if no newer resize arrived in the meantime.
                    int expected = 0;
                    pending_width_.compare_exchange_strong(expected, pw);
                    expected = 0;
                    pending_height_.compare_exchange_strong(expected, ph);
                }
            }
        }

#ifdef _WIN32
        if (auto* output = headless_output()) {
            output->cleanup_expired_pending_buffers();
        }
#endif

        bool playing_snapshot;
        bool clock_paused_snapshot;
        bool log_preroll_transition = false;
        bool log_preroll_pending = false;
        bool log_preroll_complete = false;
        double preroll_complete_pts_s = -1.0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            playing_snapshot = playing_;

            // Preroll: keep clock paused while any track is still buffering.
            const bool any_buffering = has_preroll_blocking_track(tracks_);

            // Detect Buffering -> Ready transition: force preview redraw so the
            // newly-ready track's first frame appears on screen immediately
            // (even while paused -- matches initialize() behavior).
            if (was_buffering_ && !any_buffering) {
                preview_drawn_ = false;
                log_preroll_transition = true;
            }
            was_buffering_ = any_buffering;

            clock_paused_snapshot = playback_->clock().is_paused();
            if (any_buffering && !clock_paused_snapshot) {
                playback_->clock().pause();
                clock_paused_snapshot = true;
                log_preroll_pending = true;
            } else if (!any_buffering && clock_paused_snapshot && playing_snapshot) {
                set_decode_paused_for_all_tracks(false);
                playback_->clock().resume();
                preview_drawn_ = false;
                clock_paused_snapshot = false;
                preroll_complete_pts_s =
                    static_cast<double>(playback_->clock().current_pts_us()) / 1e6;
                log_preroll_complete = true;
            }
        }
        if (log_preroll_transition) {
            spdlog::info("[Renderer] Preroll transition complete, forcing preview redraw");
        }
        if (log_preroll_pending) {
            spdlog::info("[Renderer] Preroll: clock PENDING, some track buffering, "
                         "(playing={})", playing_snapshot);
        }
        if (log_preroll_complete) {
            spdlog::info("[Renderer] === Preroll COMPLETE: all tracks ready, clock resumed, "
                         "playing_={}, pts={:.3f}s)",
                         playing_snapshot, preroll_complete_pts_s);
        }

        if (!playing_snapshot || clock_paused_snapshot) {
            if (const auto backoff =
                    transient_presentation_backpressure_remaining();
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
                should_draw_preview = !preview_drawn_ && !preview_draw_pending_;
                if (should_draw_preview) {
                    preview_draw_pending_ = true;
                }
            }
            if (should_draw_preview) {
                bool drawn = false;

                // Try cached last frame first (for layout changes while paused)
                PresentDecision cached_decision;
                std::optional<int64_t> cached_pts_us;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    filter_present_decision_against_tracks(last_decision_, tracks_);
                    if (present_decision_has_frame(last_decision_)) {
                        cached_decision = last_decision_;
                        auto available = build_available_paused_frame_snapshot(tracks_);
                        for (size_t i = 0; i < kMaxTracks; ++i) {
                            if (!cached_decision.frames[i].has_value() &&
                                available.decision.frames[i].has_value()) {
                                cached_decision.frames[i] =
                                    available.decision.frames[i];
                                cached_decision.file_ids[i] =
                                    available.decision.file_ids[i];
                                cached_decision.track_generations[i] =
                                    available.decision.track_generations[i];
                            }
                        }
                        filter_present_decision_against_tracks(
                            cached_decision, tracks_);
                        cached_pts_us =
                            first_present_decision_frame_pts_us(cached_decision);
                    }
                }
                if (present_decision_has_frame(cached_decision)) {
                    present_frame(cached_decision);
                    drawn = true;
                    spdlog::debug("[Renderer] Paused frame (cached): pts={:.3f}s",
                                  cached_pts_us.has_value()
                                      ? static_cast<double>(*cached_pts_us) / 1e6
                                      : -1.0);
                }

                // No cached frame — try track buffer (initial preview)
                // Only draw when ALL active tracks have frames, to avoid
                // flashing black for tracks that haven't finished seeking.
                if (!drawn) {
                    PausedPreviewSnapshot snapshot;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        snapshot = build_paused_preview_snapshot(tracks_);
                    }
                    if (snapshot.ready_to_present) {
                        auto& preview = snapshot.decision;
                        present_frame(preview);
                        bool preserve_requested_clock = false;
                        int ref = -1;
                        int64_t ref_offset_us = 0;
                        if (!playing_snapshot) {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            last_decision_ = preview;
                            set_decode_paused_for_all_tracks(true);
                            preserve_requested_clock = true;
                            mark_paused_hevc_seek_preview_drawn_locked();
                            ref = first_active_track();
                            if (ref >= 0 && tracks_[ref]) {
                                ref_offset_us = tracks_[ref]->offset_us;
                            }
                        } else {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            last_decision_ = preview;
                            ref = first_active_track();
                            if (ref >= 0 && tracks_[ref]) {
                                ref_offset_us = tracks_[ref]->offset_us;
                            }
                        }
                        // Keep the logical clock at the user's requested target
                        // while paused. The decoded preview can land on a
                        // nearest/tail frame for individual tracks, but the
                        // timeline should not visually snap backward.
                        if (!preserve_requested_clock &&
                            ref >= 0 &&
                            preview.frames[ref].has_value()) {
                            playback_->clock().seek(
                                preview.frames[ref]->pts_us + ref_offset_us);
                        }
                        spdlog::info("[Renderer] Paused frame: pts={:.3f}s",
                                     ref >= 0 && preview.frames[ref].has_value()
                                     ? preview.frames[ref]->pts_us / 1e6 : -1.0);
                        emit_seek_preview_presented_events(preview);
                    }
                }
                if (!drawn) {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    preview_draw_pending_ = false;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto scheduler_tick = presentation_scheduler_.tick(*render_sink_);
        auto decision = scheduler_tick.decision;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            filter_present_decision_against_tracks(decision, tracks_);
        }

        // Periodic diagnostics
        {
            auto now = std::chrono::steady_clock::now();
            int64_t pts = playback_->clock().current_pts_us();
            int64_t pts_delta = 0;
            if (render_loop_controller_.should_emit_diagnostics(now, pts, pts_delta)) {
                std::vector<RenderLoopTrackDiagnosticSnapshot> diagnostics;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    diagnostics = snapshot_render_loop_track_diagnostics(tracks_);
                }
                for (const auto& track : diagnostics) {
                    spdlog::info("[diag] track[{}]: pts={:.3f}s delta={:.1f}ms "
                                 "buf={}/{} state={} playing={}",
                                 track.slot, pts / 1e6, pts_delta / 1e3,
                                 track.buffer_count, track.buffer_capacity,
                                 static_cast<int>(track.buffer_state),
                                 playing_snapshot);
                }
            }
        }

        if (decision.should_present && scheduler_tick.should_notify) {
            if (const auto backoff =
                    transient_presentation_backpressure_remaining();
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
                apply_present_carry_forward(tracks_, last_decision_, decision);
            }
            present_frame(decision);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                last_decision_ = decision;
            }
        } else if (playing_snapshot) {
            uint64_t layout_revision = 0;
            uint64_t presented_revision = 0;
            uint64_t pending_layout_revision = 0;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                layout_revision = layout_revision_;
                presented_revision = last_presented_layout_revision_;
            }
            {
                std::lock_guard<std::mutex> lock(pending_layout_mutex_);
                pending_layout_revision = pending_layout_revision_;
            }
            const uint64_t latest_layout_revision =
                std::max(layout_revision, pending_layout_revision);
            if (latest_layout_revision > presented_revision) {
                const uint64_t pending_layout = presentation_backend_metrics_
                    .playing_layout_redraw_suppressed_count.fetch_add(
                        1, std::memory_order_relaxed) + 1;
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
                compute_empty_buffer_eof_clamp(tracks_, last_decision_);
            if (eof_clamp.all_active_buffers_empty &&
                eof_clamp.max_end_pts_us > 0) {
                int64_t current = playback_->clock().current_pts_us();
                if (current > eof_clamp.max_end_pts_us) {
                    playback_->clock().seek(eof_clamp.max_end_pts_us);
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
            int64_t current_pts = playback_->clock().current_pts_us();
            std::optional<int64_t> next_event_pts;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                next_event_pts =
                    compute_next_frame_event_pts_us(tracks_, current_pts);
            }
            if (next_event_pts.has_value()) {
                double spd = playback_->clock().speed();
                const auto sleep_for = render_loop_controller_.frame_deadline_sleep(
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

    pending_width_.store(0, std::memory_order_release);
    pending_height_.store(0, std::memory_order_release);
}

bool Renderer::draw_frame(
    const RendererDrawSnapshot& snapshot,
    const char* source,
    PresentationBackendAsyncDrawCompleted async_completion) {
    auto* backend = presentation_backend_.get();
    if (!backend) {
        return false;
    }
    PresentationBackendDrawHooks hooks;
    hooks.draw_source = source;
    hooks.wait_gpu_idle = [this](const char* label) { wait_gpu_idle(label); };
    hooks.record_frame_copy_us = [this](uint64_t elapsed_us) {
        presentation_backend_metrics_.frame_copy_us.fetch_add(
            elapsed_us, std::memory_order_relaxed);
        presentation_backend_metrics_.frame_copy_count.fetch_add(
            1, std::memory_order_relaxed);
    };
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
    hooks.async_draw_completed = std::move(async_completion);
    return backend->draw_frame(snapshot, hooks);
}

} // namespace vr
