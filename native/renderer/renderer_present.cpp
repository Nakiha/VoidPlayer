#include "renderer/renderer_internal.h"
#include "renderer/render/renderer_draw_snapshot_builder.h"
#include "renderer/render/renderer_presentation_completion.h"

namespace vr {

bool Renderer::Impl::PresentCommandProcessor::draw_paused_frame(
    Renderer::Impl& renderer,
    const char* reason) {
    auto& state_mutex_ = renderer.state_mutex_;
    auto& render_sink_ = renderer.render_sink_;
    auto& track_controller_ = renderer.track_controller_;
    auto& present_history_ = renderer.present_history_;
    const bool interactive_refresh =
        reason && (std::strcmp(reason, "macos-renderer-owned-refresh") == 0 ||
                   std::strcmp(reason, "request_frame_refresh") == 0);
    const bool decoded_preview_refresh =
        reason && std::strcmp(reason, "seek_frame_refresh") == 0;
    PresentDecision decision;
    bool has_frame = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        renderer.consume_pending_layout_locked();
        std::optional<PresentDecision> evaluated_decision;
        if (!decoded_preview_refresh && render_sink_) {
            evaluated_decision = render_sink_->evaluate();
        }
        const auto refresh_decision = track_controller_.paused_refresh_decision(
            present_history_.snapshot(), evaluated_decision, decoded_preview_refresh);
        decision = refresh_decision.decision;
        has_frame = refresh_decision.has_frame;
    }
    if (!has_frame) {
        return false;
    }
    present_frame(renderer, decision);
    int ref = -1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        track_controller_.filter_present_decision(decision);
        present_history_.set(decision);
        ref = track_controller_.first_active_slot();
    }
    double pts = (ref >= 0 && decision.frames[ref].has_value())
                 ? decision.frames[ref]->pts_us / 1e6 : -1.0;
    if (interactive_refresh) {
        spdlog::debug(
            "[Renderer] draw_paused_frame({}): pts={:.3f}s", reason, pts);
    } else {
        spdlog::info("[Renderer] draw_paused_frame({}): pts={:.3f}s", reason, pts);
    }
    return true;
}

bool Renderer::Impl::request_frame_refresh(const char* reason) {
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
        has_complete_cached_decision =
            track_controller_.has_complete_cached_decision(
                present_history_.snapshot());
        preview_draw_pending = loop_driver_.preview_pending();
    }
    if (renderer_owned_refresh) {
        if (preview_draw_pending) {
            return true;
        }
        if (PresentCommandProcessor::redraw_layout(*this)) {
            return true;
        }
        if (has_complete_cached_decision ||
            (timeline_.playing() &&
             !timeline_.playback().clock().is_paused())) {
            return false;
        }
    } else if (timeline_.playing() &&
               !timeline_.playback().clock().is_paused()) {
        return PresentCommandProcessor::redraw_layout(*this);
    }
    return PresentCommandProcessor::draw_paused_frame(*this, refresh_reason);
}

void Renderer::Impl::enter_terminal_device_lost_locked(const char* operation) {
    const auto plan = plan_renderer_device_lost_transition(
        device_state_.load(std::memory_order_acquire));
    if (!plan.apply) {
        return;
    }

    device_state_.store(plan.pre_terminal_state, std::memory_order_release);
    const long reason = presentation_.device_removed_reason();
    if (plan.count_device_lost) {
        presentation_metrics_.note_device_lost();
    }
    spdlog::error(
        "[Renderer] D3D11 device lost during {}; entering terminal renderer state "
        "(reason={:#x})",
        operation,
        static_cast<unsigned long>(reason));

    loop_driver_.set_running(false);
    timeline_.set_playing(false);
    if (plan.pause_playback) {
        timeline_.playback().pause();
    }
    if (plan.pause_decode) {
        set_decode_paused_for_all_tracks(true);
    }
    if (plan.clear_initialized) {
        initialized_ = false;
    }
    device_state_.store(plan.final_state, std::memory_order_release);
}

void Renderer::Impl::enter_terminal_render_loop_error_locked(const char* reason) {
    const auto plan = plan_renderer_runtime_error_transition(
        device_state_.load(std::memory_order_acquire));
    if (!plan.apply) {
        return;
    }
    device_state_.store(plan.pre_terminal_state, std::memory_order_release);
    spdlog::error(
        "[Renderer] Render loop entered terminal runtime state after {}",
        reason);
    loop_driver_.set_running(false);
    timeline_.set_playing(false);
    if (plan.clear_initialized) {
        initialized_ = false;
    }
    if (plan.pause_playback) {
        timeline_.playback().pause();
    }
    if (plan.pause_decode) {
        set_decode_paused_for_all_tracks(true);
    }
    device_state_.store(plan.final_state, std::memory_order_release);
}

void Renderer::Impl::PresentCommandProcessor::finish_presented_draw(
    Renderer::Impl& renderer,
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
    auto& state_mutex_ = renderer.state_mutex_;
    auto& layout_state_ = renderer.layout_state_;
    auto& presentation_metrics_ = renderer.presentation_metrics_;
    auto& loop_driver_ = renderer.loop_driver_;
    if (renderer.shutting_down_.load(std::memory_order_acquire)) {
        return;
    }

    bool stale_layout_after_draw = false;
    uint64_t current_layout_revision = snapshot_layout_revision;
    std::unique_lock<std::mutex> lock(state_mutex_, std::defer_lock);
    if (drew) {
        lock.lock();
        const auto layout_commit =
            layout_state_.commit_presented_draw(snapshot_layout_revision);
        stale_layout_after_draw = layout_commit.stale;
        current_layout_revision = layout_commit.current_revision;
        if (layout_commit.stale) {
            presentation_metrics_.note_layout_stale_completion_drop();
        }
        if (layout_commit.marked_presented) {
            presentation_metrics_.note_layout_presented();
        }
        loop_driver_.mark_preview_presented(!stale_layout_after_draw);
    } else {
        lock.lock();
        loop_driver_.clear_preview_pending();
    }
    if (lock.owns_lock()) {
        lock.unlock();
    }

    const auto completion = plan_presentation_completion({
        renderer.shutting_down_.load(std::memory_order_acquire),
        attempted_draw,
        drew,
        stale_layout_after_draw,
        static_cast<bool>(frame_callback),
        static_cast<bool>(frame_failure_callback),
        frame_failure_error,
        current_layout_revision,
        completed_frame_info,
    });

    if (completion.callback_published) {
        frame_callback(completion.callback_frame_info_ptr);
    }
    if (completion.transient_backpressure) {
        const auto count = presentation_metrics_.note_transient_backpressure(
            kTransientPresentationBackpressureBackoff,
            steady_clock_us_now());
        if (profiler_enabled("VOIDPLAYER_MACOS_PROFILER") &&
            (count <= 8 || count % 120 == 0)) {
            spdlog::info(
                "[RendererProfiler] transient presentation backpressure source={} "
                "backoff_us={} count={}",
                source ? source : "",
                kTransientPresentationBackpressureBackoff.count(),
                count);
        }
    }
    if (completion.notify_frame_failure) {
        frame_failure_callback(completion.frame_failure_error);
    }

    const auto total_us = elapsed_us_since(profiler_start);
    presentation_metrics_.record_draw_timing(total_us, backend_us);
    log_viewport_draw_trace(source,
                            snapshot,
                            snapshot_layout_revision,
                            current_layout_revision,
                            attempted_draw,
                            drew,
                            stale_layout_after_draw,
                            completion.callback_available,
                            completion.callback_published,
                            total_us,
                            snapshot_us,
                            backend_us);
}

void Renderer::Impl::PresentCommandProcessor::present_frame(
    Renderer::Impl& renderer,
    const PresentDecision& decision) {
    auto& state_mutex_ = renderer.state_mutex_;
    auto& layout_state_ = renderer.layout_state_;
    auto& track_controller_ = renderer.track_controller_;
    auto& surface_state_ = renderer.surface_state_;
    auto& presentation_metrics_ = renderer.presentation_metrics_;
    auto& presentation_ = renderer.presentation_;
    auto& loop_driver_ = renderer.loop_driver_;
    const auto profiler_start = std::chrono::steady_clock::now();
    RendererDrawSnapshot snapshot;
    uint64_t snapshot_layout_revision = 0;
    uint64_t snapshot_us = 0;
    {
        const auto snapshot_start = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (renderer.should_present_frame_consume_pending_layout()) {
            renderer.consume_pending_layout_locked();
        }
        spdlog::debug("[present_frame] mode={}", layout_state_.current_for_draw().mode);
        PresentDecision filtered_decision = decision;
        track_controller_.filter_present_decision(filtered_decision);
        RendererDrawSnapshotBuilder::update_track_geometry_from_decision(
            track_controller_, filtered_decision);
        snapshot = RendererDrawSnapshotBuilder::build(track_controller_,
                                                      layout_state_,
                                                      surface_state_,
                                                      filtered_decision);
        snapshot_layout_revision = layout_state_.current_revision();
        snapshot_us = elapsed_us_since(snapshot_start);
    }
    const bool attempted_draw = present_decision_has_frame(snapshot.decision);
    if (attempted_draw &&
        renderer.should_suppress_playback_present_for_viewport_compositor()) {
        const auto suppressed =
            presentation_metrics_.note_playing_layout_redraw_suppressed();
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
    RendererPresentationSubmitRequest request(snapshot, presentation_metrics_);
    request.source = "present_frame";
    request.headless = surface_state_.headless();
    request.publish_swap_chain_after_sync_draw = true;
    request.poll_device_removed_label =
        surface_state_.headless() ? "headless present" : nullptr;
    request.check_device_lost_after_draw = !surface_state_.headless();
    request.overlay_hooks = renderer.presentation_overlay_hooks();
    request.should_abort_headless_publish =
        [&renderer]() {
            return renderer.shutting_down_.load(std::memory_order_acquire);
        };
    request.async_completed =
        [&renderer,
         snapshot,
         snapshot_layout_revision,
        snapshot_us,
        profiler_start,
        attempted_draw](const RendererPresentationAsyncCompletion& completion) {
            finish_presented_draw(
                renderer,
                "present_frame",
                snapshot,
                snapshot_layout_revision,
                snapshot_us,
                profiler_start,
                attempted_draw,
                completion.callbacks.frame_callback,
                completion.callbacks.frame_failure_callback,
                completion.success,
                completion.error,
                completion.backend_us,
                completion.frame_info);
        };
    RendererPresentationDrawResult sync_draw_result;
    bool sync_completed = false;
    presentation_.submit_and_dispatch(
        std::move(request),
        RendererPresentationSubmitDispatchHooks{
            [&renderer]() {
                std::lock_guard<std::mutex> lock(renderer.state_mutex_);
                renderer.enter_terminal_device_lost_locked("present_frame");
            },
            [&renderer]() {
                std::lock_guard<std::mutex> lock(renderer.state_mutex_);
                if (!renderer.timeline_.playing() ||
                    renderer.timeline_.playback().clock().is_paused()) {
                    renderer.loop_driver_.mark_async_preview_pending();
                }
            },
            [&renderer,
             &sync_completed,
             &sync_draw_result,
             &snapshot,
             snapshot_layout_revision,
             snapshot_us,
             profiler_start,
             attempted_draw](const RendererPresentationSyncCompletion& completion) {
                sync_completed = true;
                sync_draw_result = completion.draw;
                finish_presented_draw(
                    renderer,
                    "present_frame",
                    snapshot,
                    snapshot_layout_revision,
                    snapshot_us,
                    profiler_start,
                    attempted_draw,
                    completion.draw.frame_callback,
                    completion.callbacks.frame_failure_callback,
                    completion.draw.drew,
                    completion.draw.failure_error.c_str(),
                    completion.draw.backend_us,
                    nullptr);
            },
        });
    if (!sync_completed) {
        return;
    }
    const auto total_us = elapsed_us_since(profiler_start);
    static std::atomic<uint64_t> present_profiler_count{0};
    const auto count = present_profiler_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (profiler_enabled("VOIDPLAYER_MACOS_PROFILER") &&
        (total_us >= 8000 || sync_draw_result.backend_us >= 6000 ||
         count % 240 == 0)) {
        size_t active_tracks = 0;
        bool final_preview_drawn = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            active_tracks = track_controller_.count();
            final_preview_drawn = loop_driver_.preview_drawn();
        }
        spdlog::info(
            "[RendererProfiler] present_frame total_us={} snapshot_us={} backend_us={} "
            "attempted={} drew={} headless={} tracks={} layout_rev={} preview_drawn={}",
            total_us,
            snapshot_us,
            sync_draw_result.backend_us,
            attempted_draw,
            sync_draw_result.drew,
            surface_state_.headless(),
            active_tracks,
            snapshot_layout_revision,
            final_preview_drawn);
    }
}

bool Renderer::Impl::PresentCommandProcessor::redraw_layout(
    Renderer::Impl& renderer) {
    auto& presentation_metrics_ = renderer.presentation_metrics_;
    auto& state_mutex_ = renderer.state_mutex_;
    auto& track_controller_ = renderer.track_controller_;
    auto& present_history_ = renderer.present_history_;
    auto& layout_state_ = renderer.layout_state_;
    auto& surface_state_ = renderer.surface_state_;
    auto& presentation_ = renderer.presentation_;
    if (presentation_metrics_
            .transient_backpressure_remaining(steady_clock_us_now())
            .count() > 0) {
        return false;
    }
    const auto profiler_start = std::chrono::steady_clock::now();
    RendererDrawSnapshot snapshot;
    uint64_t snapshot_layout_revision = 0;
    uint64_t snapshot_us = 0;
    {
        const auto snapshot_start = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(state_mutex_);
        renderer.consume_pending_layout_locked();
        const auto decision_result =
            track_controller_.paused_layout_decision(present_history_.snapshot());
        snapshot_layout_revision = layout_state_.current_revision();
        snapshot_us = elapsed_us_since(snapshot_start);
        if (!decision_result.has_frame) {
            if (profiler_enabled("VOIDPLAYER_MACOS_PROFILER")) {
                spdlog::info(
                    "[RendererProfiler] redraw_layout skipped: no complete "
                    "decision layout_rev={} tracks={}",
                    snapshot_layout_revision,
                    decision_result.active_track_count);
            }
            return false;
        }
        const auto& decision = decision_result.decision;
        present_history_.set(decision);
        RendererDrawSnapshotBuilder::update_track_geometry_from_decision(
            track_controller_, decision);
        snapshot = RendererDrawSnapshotBuilder::build(track_controller_,
                                                      layout_state_,
                                                      surface_state_,
                                                      decision);
    }
    const bool attempted_draw = present_decision_has_frame(snapshot.decision);
    RendererPresentationSubmitRequest request(snapshot, presentation_metrics_);
    request.source = "viewport_composite";
    request.headless = surface_state_.headless();
    request.wait_idle_after_sync_draw_label =
        surface_state_.headless() ? nullptr : "viewport_composite";
    request.poll_device_removed_label =
        surface_state_.headless() ? "headless redraw" : "viewport_composite";
    request.overlay_hooks = renderer.presentation_overlay_hooks();
    request.should_abort_headless_publish =
        [&renderer]() {
            return renderer.shutting_down_.load(std::memory_order_acquire);
        };
    request.async_completed =
        [&renderer,
         snapshot,
         snapshot_layout_revision,
         snapshot_us,
         profiler_start,
         attempted_draw](const RendererPresentationAsyncCompletion& completion) {
            finish_presented_draw(renderer,
                                  "viewport_composite",
                                  snapshot,
                                  snapshot_layout_revision,
                                  snapshot_us,
                                  profiler_start,
                                  attempted_draw,
                                  completion.callbacks.frame_callback,
                                  completion.callbacks.frame_failure_callback,
                                  completion.success,
                                  completion.error,
                                  completion.backend_us,
                                  completion.frame_info);
        };
    return presentation_.submit_and_dispatch(
        std::move(request),
        RendererPresentationSubmitDispatchHooks{
            [&renderer]() {
                std::lock_guard<std::mutex> lock(renderer.state_mutex_);
                renderer.enter_terminal_device_lost_locked("viewport_composite");
            },
            [&renderer]() {
                std::lock_guard<std::mutex> lock(renderer.state_mutex_);
                if (!renderer.timeline_.playing() ||
                    renderer.timeline_.playback().clock().is_paused()) {
                    renderer.loop_driver_.mark_async_preview_pending();
                }
            },
            [&renderer,
             &snapshot,
             snapshot_layout_revision,
             snapshot_us,
             profiler_start,
             attempted_draw](const RendererPresentationSyncCompletion& completion) {
                finish_presented_draw(renderer,
                                      "viewport_composite",
                                      snapshot,
                                      snapshot_layout_revision,
                                      snapshot_us,
                                      profiler_start,
                                      attempted_draw,
                                      completion.draw.frame_callback,
                                      completion.callbacks.frame_failure_callback,
                                      completion.draw.drew,
                                      completion.draw.failure_error.c_str(),
                                      completion.draw.backend_us,
                                      nullptr);
            },
        });
}

bool Renderer::Impl::capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.headless()) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    return presentation_.capture_d3d_headless_front_buffer(bgra, width, height);
#else
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.headless()) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    return presentation_.capture_backend_front_buffer(bgra, width, height);
#endif
}

} // namespace vr
