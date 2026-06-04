#include "renderer/renderer_internal.h"

namespace vr {

bool Renderer::draw_paused_frame(const char* reason) {
    const bool interactive_refresh =
        reason && (std::strcmp(reason, "macos-renderer-owned-refresh") == 0 ||
                   std::strcmp(reason, "request_frame_refresh") == 0);
    const bool decoded_preview_refresh =
        reason && std::strcmp(reason, "seek_frame_refresh") == 0;
    PresentDecision decision;
    bool has_frame = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        consume_pending_layout_locked();
        if (decoded_preview_refresh) {
            const auto snapshot = build_paused_preview_snapshot(tracks_);
            if (snapshot.ready_to_present) {
                decision = snapshot.decision;
                has_frame = true;
            }
        } else {
            filter_present_decision_against_tracks(last_decision_, tracks_);
        }
        if (!decoded_preview_refresh && render_sink_) {
            decision = render_sink_->evaluate();
            filter_present_decision_against_tracks(decision, tracks_);
            if (decision.should_present) {
                apply_present_carry_forward(tracks_, last_decision_, decision);
            }
        }
        has_frame = present_decision_has_frame(decision);
        if (!decoded_preview_refresh && !has_frame &&
            present_decision_has_frame(last_decision_)) {
            decision = last_decision_;
            has_frame = true;
        }
        if (has_frame) {
            auto available = build_available_paused_frame_snapshot(tracks_);
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!decision.frames[i].has_value() &&
                    available.decision.frames[i].has_value()) {
                    decision.frames[i] = available.decision.frames[i];
                    decision.file_ids[i] = available.decision.file_ids[i];
                    decision.track_generations[i] =
                        available.decision.track_generations[i];
                }
            }
            filter_present_decision_against_tracks(decision, tracks_);
            has_frame = present_decision_has_frame(decision);
        }

        if (has_frame && active_track_count(tracks_) > 1 &&
            !present_decision_covers_active_tracks(decision, tracks_)) {
            // Paused refreshes are often driven by seek/layout/EOF paths where
            // tracks can become ready at different times.  Do not let a partial
            // refresh overwrite a complete cached decision; otherwise a slower
            // secondary track can disappear until the next full playback tick.
            if (!decoded_preview_refresh &&
                present_decision_covers_active_tracks(last_decision_, tracks_)) {
                decision = last_decision_;
            } else {
                const auto snapshot = build_paused_preview_snapshot(tracks_);
                if (snapshot.ready_to_present) {
                    decision = snapshot.decision;
                    has_frame = true;
                } else {
                    has_frame = false;
                }
            }
        }
    }
    if (!has_frame) {
        return false;
    }
    present_frame(decision);
    int ref = -1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        filter_present_decision_against_tracks(decision, tracks_);
        last_decision_ = decision;
        ref = first_active_track();
    }
    double pts = (ref >= 0 && decision.frames[ref].has_value())
                 ? decision.frames[ref]->pts_us / 1e6 : -1.0;
    if (interactive_refresh) {
        spdlog::debug("[Renderer] draw_paused_frame({}): pts={:.3f}s", reason, pts);
    } else {
        spdlog::info("[Renderer] draw_paused_frame({}): pts={:.3f}s", reason, pts);
    }
    return true;
}

bool Renderer::request_frame_refresh(const char* reason) {
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!initialized_.load(std::memory_order_acquire) ||
            shutting_down_.load(std::memory_order_acquire)) {
            return false;
        }
    }
    const char* refresh_reason = reason && reason[0] != '\0'
                                     ? reason
                                     : "request_frame_refresh";
    const bool renderer_owned_refresh =
        std::strcmp(refresh_reason, "macos-renderer-owned-refresh") == 0 ||
        std::strcmp(refresh_reason, "request_frame_refresh") == 0;
    const bool viewport_compositor_refresh =
        std::strcmp(refresh_reason, "macos-renderer-owned-refresh") == 0;
    if (viewport_compositor_refresh) {
        note_viewport_compositor_activity();
    }
    bool has_complete_cached_decision = false;
    bool preview_draw_pending = false;
    if (renderer_owned_refresh) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        PresentDecision cached = last_decision_;
        filter_present_decision_against_tracks(cached, tracks_);
        const auto active_count = active_track_count(tracks_);
        has_complete_cached_decision =
            present_decision_has_frame(cached) &&
            (active_count <= 1 ||
             present_decision_covers_active_tracks(cached, tracks_));
        preview_draw_pending = preview_draw_pending_;
    }
    if (renderer_owned_refresh) {
        if (preview_draw_pending) {
            return true;
        }
        if (redraw_layout()) {
            return true;
        }
        if (has_complete_cached_decision ||
            (playing_.load(std::memory_order_acquire) &&
             !playback_->clock().is_paused())) {
            return false;
        }
    } else if (playing_.load(std::memory_order_acquire) &&
               !playback_->clock().is_paused()) {
        return redraw_layout();
    }
    return draw_paused_frame(refresh_reason);
}

RendererDrawSnapshot Renderer::build_draw_snapshot_locked(
    const PresentDecision& decision) const {
    RendererDrawSnapshot snapshot;
    snapshot.decision = decision;
    filter_present_decision_against_tracks(snapshot.decision, tracks_);
    snapshot.layout = layout_;
    snapshot.track_geometry = snapshot_layout_track_geometry(tracks_);
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) {
            continue;
        }
        auto& out = snapshot.tracks[i];
        out.active = true;
        out.file_id = tracks_[i]->file_id;
        out.generation = tracks_[i]->generation;
        out.offset_us = tracks_[i]->offset_us;
        out.video_width = tracks_[i]->video_width;
        out.video_height = tracks_[i]->video_height;
        out.video_aspect = tracks_[i]->video_aspect;
    }
    for (int i = 0; i < 4; ++i) {
        snapshot.background_color[i] = background_color_[i];
    }
    snapshot.target_width = target_width_;
    snapshot.target_height = target_height_;
    return snapshot;
}

void Renderer::wait_gpu_idle(const char* label) {
    const auto start = std::chrono::steady_clock::now();
    if (presentation_backend_) {
        presentation_backend_->wait_idle(label);
    }
    presentation_backend_metrics_.render_wait_us.fetch_add(
        elapsed_us_since(start), std::memory_order_relaxed);
    presentation_backend_metrics_.render_wait_count.fetch_add(1, std::memory_order_relaxed);
}

bool Renderer::draw_headless_and_publish(const RendererDrawSnapshot& snapshot,
                                         const char* label,
                                         RendererFrameCallback& callback) {
#ifdef _WIN32
    callback = {};
    if (shutting_down_.load(std::memory_order_acquire)) {
        return false;
    }
    auto* output = headless_output();
    auto* resources = d3d_resources();
    if (!output || !resources) {
        return false;
    }
    {
        std::lock_guard<std::mutex> tex_lock(texture_mutex());
        auto* rtv = output->begin_frame_locked();
        if (!rtv) {
            return false;
        }
        resources->cached_rtv = rtv;
    }
    if (!draw_frame(snapshot, label)) {
        return false;
    }
    const auto publish_start = std::chrono::steady_clock::now();
    output->wait_gpu_idle(label);
    {
        std::lock_guard<std::mutex> tex_lock(texture_mutex());
        auto published_callback = output->publish_frame_locked();
        callback = published_callback
            ? RendererFrameCallback(
                  [published_callback = std::move(published_callback)](
                      const PresentationBackendFrameInfo*) mutable {
                      published_callback();
                  })
            : RendererFrameCallback();
    }
    if (shutting_down_.load(std::memory_order_acquire)) {
        callback = {};
    }
    presentation_backend_metrics_.present_publish_us.fetch_add(
        elapsed_us_since(publish_start), std::memory_order_relaxed);
    presentation_backend_metrics_.present_publish_count.fetch_add(1, std::memory_order_relaxed);
    return true;
#else
    (void)snapshot;
    (void)label;
    callback = {};
    return false;
#endif
}

void Renderer::enter_terminal_device_lost_locked(const char* operation) {
    const auto plan = plan_renderer_device_lost_transition(
        device_state_.load(std::memory_order_acquire));
    if (!plan.apply) {
        return;
    }

    device_state_.store(plan.pre_terminal_state, std::memory_order_release);
    const long reason = presentation_backend_
        ? presentation_backend_->device_removed_reason()
        : 0;
    if (plan.count_device_lost) {
        presentation_backend_metrics_.device_lost_count.fetch_add(1, std::memory_order_relaxed);
    }
    spdlog::error(
        "[Renderer] D3D11 device lost during {}; entering terminal renderer state "
        "(reason={:#x})",
        operation,
        static_cast<unsigned long>(reason));

    running_ = false;
    playing_ = false;
    if (plan.pause_playback) {
        playback_->pause();
    }
    if (plan.pause_decode) {
        set_decode_paused_for_all_tracks(true);
    }
    if (plan.clear_initialized) {
        initialized_ = false;
    }
    device_state_.store(plan.final_state, std::memory_order_release);
}

void Renderer::enter_terminal_render_loop_error_locked(const char* reason) {
    const auto plan = plan_renderer_runtime_error_transition(
        device_state_.load(std::memory_order_acquire));
    if (!plan.apply) {
        return;
    }
    device_state_.store(plan.pre_terminal_state, std::memory_order_release);
    spdlog::error(
        "[Renderer] Render loop entered terminal runtime state after {}",
        reason);
    running_ = false;
    playing_ = false;
    if (plan.clear_initialized) {
        initialized_ = false;
    }
    if (plan.pause_playback) {
        playback_->pause();
    }
    if (plan.pause_decode) {
        set_decode_paused_for_all_tracks(true);
    }
    device_state_.store(plan.final_state, std::memory_order_release);
}

void Renderer::finish_presented_draw(
    const char* source,
    const RendererDrawSnapshot& snapshot,
    uint64_t snapshot_layout_revision,
    uint64_t snapshot_us,
    std::chrono::steady_clock::time_point profiler_start,
    bool attempted_draw,
    RendererFrameCallback frame_callback,
    std::function<void(const char*)> frame_failure_callback,
    bool drew,
    const char* frame_failure_error,
    uint64_t backend_us,
    const PresentationBackendFrameInfo* completed_frame_info) {
    if (shutting_down_.load(std::memory_order_acquire)) {
        return;
    }

    bool stale_layout_after_draw = false;
    uint64_t current_layout_revision = snapshot_layout_revision;
    std::unique_lock<std::mutex> lock(state_mutex_, std::defer_lock);
    if (drew) {
        lock.lock();
        stale_layout_after_draw = snapshot_layout_revision != layout_revision_;
        current_layout_revision = layout_revision_;
        if (stale_layout_after_draw) {
            presentation_backend_metrics_.layout_stale_completion_drop_count.fetch_add(
                1, std::memory_order_relaxed);
        }
        if (snapshot_layout_revision > last_presented_layout_revision_) {
            last_presented_layout_revision_ = snapshot_layout_revision;
            presentation_backend_metrics_.layout_presented_count.fetch_add(
                1, std::memory_order_relaxed);
        }
        preview_drawn_ = !stale_layout_after_draw;
        preview_draw_pending_ = false;
    } else {
        lock.lock();
        preview_draw_pending_ = false;
    }
    if (lock.owns_lock()) {
        lock.unlock();
    }

    PresentationBackendFrameInfo callback_frame_info;
    const PresentationBackendFrameInfo* callback_frame_info_ptr = completed_frame_info;
    if (completed_frame_info) {
        callback_frame_info = *completed_frame_info;
        callback_frame_info.layout_revision = current_layout_revision;
        callback_frame_info_ptr = &callback_frame_info;
    }

    const bool callback_available = static_cast<bool>(frame_callback);
    const bool callback_published =
        callback_available && !stale_layout_after_draw &&
        !shutting_down_.load(std::memory_order_acquire);
    if (callback_published) {
        frame_callback(callback_frame_info_ptr);
    }
    const bool transient_backpressure =
        frame_failure_error &&
        is_transient_presentation_backpressure_error(frame_failure_error);
    if (transient_backpressure) {
        note_transient_presentation_backpressure(source);
    }
    if (attempted_draw && !drew && !transient_backpressure && frame_failure_callback &&
        !shutting_down_.load(std::memory_order_acquire)) {
        frame_failure_callback(frame_failure_error ? frame_failure_error : "");
    }

    const auto total_us = elapsed_us_since(profiler_start);
    record_presentation_draw_timing(total_us, backend_us);
    log_viewport_draw_trace(source,
                            snapshot,
                            snapshot_layout_revision,
                            current_layout_revision,
                            attempted_draw,
                            drew,
                            stale_layout_after_draw,
                            callback_available,
                            callback_published,
                            total_us,
                            snapshot_us,
                            backend_us);
}

void Renderer::present_frame(const PresentDecision& decision) {
    const auto profiler_start = std::chrono::steady_clock::now();
    RendererDrawSnapshot snapshot;
    uint64_t snapshot_layout_revision = 0;
    uint64_t snapshot_us = 0;
    {
        const auto snapshot_start = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (should_present_frame_consume_pending_layout()) {
            consume_pending_layout_locked();
        }
        spdlog::debug("[present_frame] mode={}", layout_.mode);
        PresentDecision filtered_decision = decision;
        filter_present_decision_against_tracks(filtered_decision, tracks_);
        update_track_geometry_from_decision_locked(filtered_decision);
        snapshot = build_draw_snapshot_locked(filtered_decision);
        snapshot_layout_revision = layout_revision_;
        snapshot_us = elapsed_us_since(snapshot_start);
    }
    RendererFrameCallback frame_callback;
    auto frame_failure_callback = frame_failure_callback_snapshot();
    std::string frame_failure_error;
    const bool attempted_draw = present_decision_has_frame(snapshot.decision);
    if (attempted_draw &&
        should_suppress_playback_present_for_viewport_compositor()) {
        const auto suppressed = presentation_backend_metrics_
                                    .playing_layout_redraw_suppressed_count.fetch_add(
                                        1, std::memory_order_relaxed) +
                                1;
        if (profiler_enabled("VOIDPLAYER_MACOS_PROFILER") &&
            (suppressed % 120 == 0 || viewport_trace_log_all())) {
            spdlog::info(
                "[RendererProfiler] present_frame suppressed by viewport compositor "
                "layout_rev={} suppressed={} tracks={}",
                snapshot_layout_revision,
                suppressed,
                present_decision_frame_count(snapshot.decision));
        }
        return;
    }
    bool device_lost = false;
    bool drew = false;
    bool async_draw_submitted = false;
    uint64_t backend_us = 0;
    {
        const auto backend_start = std::chrono::steady_clock::now();
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        auto* backend = presentation_backend_.get();
        const bool async_backend = backend && backend->completes_draw_asynchronously();
        auto async_completion =
            async_backend
                ? PresentationBackendAsyncDrawCompleted(
                      [this,
                       snapshot,
                       snapshot_layout_revision,
                       snapshot_us,
                       profiler_start,
                       attempted_draw,
                       frame_callback = frame_callback_,
                       frame_failure_callback](bool success,
                                               const char* error,
                                               uint64_t completion_backend_us,
                                               const PresentationBackendFrameInfo* frame_info) {
                          finish_presented_draw("present_frame",
                                                snapshot,
                                                snapshot_layout_revision,
                                                snapshot_us,
                                                profiler_start,
                                                attempted_draw,
                                                frame_callback,
                                                frame_failure_callback,
                                                success,
                                                error,
                                                completion_backend_us,
                                                frame_info);
                      })
                : PresentationBackendAsyncDrawCompleted();
        if (headless_) {
            if (backend && backend->renderer_manages_headless_publish()) {
                drew = draw_headless_and_publish(snapshot, "present_frame", frame_callback);
            } else {
                drew = draw_frame(snapshot, "present_frame", async_completion);
                async_draw_submitted = async_backend && drew;
                if (drew && !async_draw_submitted) {
                    frame_callback = frame_callback_;
                }
            }
            device_lost = backend && backend->poll_device_removed("headless present");
        } else {
            drew = draw_frame(snapshot, "present_frame", async_completion);
            async_draw_submitted = async_backend && drew;
            if (should_present_swap_chain_after_draw(
                    drew && !async_draw_submitted,
                    backend && backend->supports_swap_chain_present())) {
                const auto present_start = std::chrono::steady_clock::now();
                const bool presented = backend->present_swap_chain(0);
                presentation_backend_metrics_.present_publish_us.fetch_add(
                    elapsed_us_since(present_start), std::memory_order_relaxed);
                presentation_backend_metrics_.present_publish_count.fetch_add(
                    1, std::memory_order_relaxed);
                device_lost = !presented && backend->device_lost();
            } else {
                device_lost = backend && backend->device_lost();
            }
        }
        if (attempted_draw && !drew) {
            frame_failure_error = presentation_backend_last_error();
        }
        backend_us = elapsed_us_since(backend_start);
    }
    if (device_lost) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_device_lost_locked("present_frame");
        return;
    }
    if (async_draw_submitted) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!playing_.load(std::memory_order_acquire) ||
            playback_->clock().is_paused()) {
            preview_draw_pending_ = true;
        }
        return;
    }

    finish_presented_draw("present_frame",
                          snapshot,
                          snapshot_layout_revision,
                          snapshot_us,
                          profiler_start,
                          attempted_draw,
                          frame_callback,
                          frame_failure_callback,
                          drew,
                          frame_failure_error.c_str(),
                          backend_us,
                          nullptr);
    const auto total_us = elapsed_us_since(profiler_start);
    static std::atomic<uint64_t> present_profiler_count{0};
    const auto count = present_profiler_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (profiler_enabled("VOIDPLAYER_MACOS_PROFILER") &&
        (total_us >= 8000 || backend_us >= 6000 || count % 240 == 0)) {
        size_t active_tracks = 0;
        bool final_preview_drawn = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            active_tracks = active_track_count(tracks_);
            final_preview_drawn = preview_drawn_;
        }
        spdlog::info(
            "[RendererProfiler] present_frame total_us={} snapshot_us={} backend_us={} "
            "attempted={} drew={} headless={} tracks={} layout_rev={} preview_drawn={}",
            total_us,
            snapshot_us,
            backend_us,
            attempted_draw,
            drew,
            headless_,
            active_tracks,
            snapshot_layout_revision,
            final_preview_drawn);
    }
}

bool Renderer::redraw_layout() {
    if (transient_presentation_backpressure_remaining().count() > 0) {
        return false;
    }
    const auto profiler_start = std::chrono::steady_clock::now();
    RendererDrawSnapshot snapshot;
    uint64_t snapshot_layout_revision = 0;
    uint64_t snapshot_us = 0;
    {
        const auto snapshot_start = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(state_mutex_);
        consume_pending_layout_locked();
        PresentDecision decision = last_decision_;
        filter_present_decision_against_tracks(decision, tracks_);
        bool has_frame = present_decision_has_frame(decision);
        if (has_frame) {
            auto available = build_available_paused_frame_snapshot(tracks_);
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!decision.frames[i].has_value() &&
                    available.decision.frames[i].has_value()) {
                    decision.frames[i] = available.decision.frames[i];
                    decision.file_ids[i] = available.decision.file_ids[i];
                    decision.track_generations[i] =
                        available.decision.track_generations[i];
                }
            }
            filter_present_decision_against_tracks(decision, tracks_);
            has_frame = present_decision_has_frame(decision);
        }
        if (has_frame && active_track_count(tracks_) > 1 &&
            !present_decision_covers_active_tracks(decision, tracks_)) {
            const auto preview = build_paused_preview_snapshot(tracks_);
            if (preview.ready_to_present) {
                decision = preview.decision;
                has_frame = true;
            } else {
                has_frame = false;
            }
        }
        snapshot_layout_revision = layout_revision_;
        snapshot_us = elapsed_us_since(snapshot_start);
        if (!has_frame) {
            if (profiler_enabled("VOIDPLAYER_MACOS_PROFILER")) {
                spdlog::info(
                    "[RendererProfiler] redraw_layout skipped: no complete "
                    "decision layout_rev={} tracks={}",
                    snapshot_layout_revision,
                    active_track_count(tracks_));
            }
            return false;
        }
        last_decision_ = decision;
        update_track_geometry_from_decision_locked(decision);
        snapshot = build_draw_snapshot_locked(decision);
    }
    RendererFrameCallback frame_callback;
    auto frame_failure_callback = frame_failure_callback_snapshot();
    std::string frame_failure_error;
    const bool attempted_draw = present_decision_has_frame(snapshot.decision);
    bool device_lost = false;
    bool drew = false;
    bool async_draw_submitted = false;
    uint64_t backend_us = 0;
    {
        const auto backend_start = std::chrono::steady_clock::now();
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        auto* backend = presentation_backend_.get();
        const bool async_backend = backend && backend->completes_draw_asynchronously();
        auto async_completion =
            async_backend
                ? PresentationBackendAsyncDrawCompleted(
                      [this,
                       snapshot,
                       snapshot_layout_revision,
                       snapshot_us,
                       profiler_start,
                       attempted_draw,
                       frame_callback = frame_callback_,
                       frame_failure_callback](bool success,
                                               const char* error,
                                               uint64_t completion_backend_us,
                                               const PresentationBackendFrameInfo* frame_info) {
                          finish_presented_draw("viewport_composite",
                                                snapshot,
                                                snapshot_layout_revision,
                                                snapshot_us,
                                                profiler_start,
                                                attempted_draw,
                                                frame_callback,
                                                frame_failure_callback,
                                                success,
                                                error,
                                                completion_backend_us,
                                                frame_info);
                      })
                : PresentationBackendAsyncDrawCompleted();
        if (headless_) {
            if (backend && backend->renderer_manages_headless_publish()) {
                drew = draw_headless_and_publish(snapshot, "viewport_composite", frame_callback);
            } else {
                drew = draw_frame(snapshot, "viewport_composite", async_completion);
                async_draw_submitted = async_backend && drew;
                if (drew && !async_draw_submitted) {
                    frame_callback = frame_callback_;
                }
            }
            device_lost = backend && backend->poll_device_removed("headless redraw");
        } else if (backend) {
            drew = draw_frame(snapshot, "viewport_composite", async_completion);
            async_draw_submitted = async_backend && drew;
            if (!async_draw_submitted) {
                backend->wait_idle("viewport_composite");
            }
            device_lost = backend->poll_device_removed("viewport_composite");
        }
        if (attempted_draw && !drew) {
            frame_failure_error = presentation_backend_last_error();
        }
        backend_us = elapsed_us_since(backend_start);
    }
    if (device_lost) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_device_lost_locked("viewport_composite");
        return false;
    }
    if (async_draw_submitted) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!playing_.load(std::memory_order_acquire) ||
            playback_->clock().is_paused()) {
            preview_draw_pending_ = true;
        }
        return true;
    }
    finish_presented_draw("viewport_composite",
                          snapshot,
                          snapshot_layout_revision,
                          snapshot_us,
                          profiler_start,
                          attempted_draw,
                          frame_callback,
                          frame_failure_callback,
                          drew,
                          frame_failure_error.c_str(),
                          backend_us,
                          nullptr);
    return drew;
}

bool Renderer::capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    auto* output = headless_output();
    if (!headless_ || !output) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    if (!frame_capture_) {
        return false;
    }
    return frame_capture_->capture_headless_front_buffer(
        *output, device_mutex_, bgra, width, height);
#else
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    if (presentation_backend_ &&
        presentation_backend_->capture_front_buffer(bgra, width, height)) {
        return true;
    }
    bgra.clear();
    width = 0;
    height = 0;
    return false;
#endif
}

} // namespace vr
