#include "renderer/render/renderer_present_command.h"

#include "renderer/render/renderer_draw_snapshot_builder.h"
#include "renderer/render/renderer_profiler_flags.h"
#include "renderer/render/renderer_presentation_completion.h"
#include "renderer/render/renderer_timing_utils.h"
#include "renderer/render/renderer_viewport_trace.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>

#include <spdlog/spdlog.h>

namespace vr {
namespace {

struct RendererPresentCompletionContext {
    std::mutex* state_mutex = nullptr;
    RendererLayoutState* layout = nullptr;
    RendererPresentHistory* history = nullptr;
    PresentationMetricsStore* metrics = nullptr;
    RendererLoopDriver* loop = nullptr;
    std::atomic<bool>* shutting_down = nullptr;
};

void finish_presented_draw(
    RendererPresentCompletionContext context,
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
    if (context.shutting_down->load(std::memory_order_acquire)) {
        return;
    }

    bool stale_layout_after_draw = false;
    uint64_t current_layout_revision = snapshot_layout_revision;
    std::unique_lock<std::mutex> lock(*context.state_mutex, std::defer_lock);
    if (drew) {
        lock.lock();
        const auto layout_commit =
            context.layout->commit_presented_draw(snapshot_layout_revision);
        stale_layout_after_draw = layout_commit.stale;
        current_layout_revision = layout_commit.current_revision;
        if (layout_commit.stale) {
            context.metrics->note_layout_stale_completion_drop();
        }
        if (layout_commit.marked_presented) {
            context.metrics->note_layout_presented();
        }
        context.loop->mark_preview_presented(!stale_layout_after_draw);
    } else {
        lock.lock();
        context.loop->clear_preview_pending();
    }
    if (lock.owns_lock()) {
        lock.unlock();
    }

    const auto completion = plan_presentation_completion({
        context.shutting_down->load(std::memory_order_acquire),
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
        const auto count = context.metrics->note_transient_backpressure(
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
    context.metrics->record_draw_timing(total_us, backend_us);
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

RendererPresentCompletionContext completion_context(
    RendererPresentCommandContext& context) {
    return RendererPresentCompletionContext{
        &context.state_mutex,
        &context.layout,
        &context.history,
        &context.metrics,
        &context.loop,
        &context.shutting_down,
    };
}

RendererPresentationSubmitDispatchHooks dispatch_hooks(
    RendererPresentCommandContext& context,
    RendererPresentCompletionContext completion_ctx,
    const char* source,
    const RendererDrawSnapshot& snapshot,
    uint64_t snapshot_layout_revision,
    uint64_t snapshot_us,
    std::chrono::steady_clock::time_point profiler_start,
    bool attempted_draw,
    bool* sync_completed,
    RendererPresentationDrawResult* sync_draw_result) {
    auto enter_terminal_device_lost =
        context.hooks.enter_terminal_device_lost_locked;
    auto playback_inactive_or_paused =
        context.hooks.playback_inactive_or_paused;
    return RendererPresentationSubmitDispatchHooks{
        [&context, enter_terminal_device_lost, source]() {
            std::lock_guard<std::mutex> lock(context.state_mutex);
            enter_terminal_device_lost(source);
        },
        [&context, playback_inactive_or_paused]() {
            std::lock_guard<std::mutex> lock(context.state_mutex);
            if (playback_inactive_or_paused()) {
                context.loop.mark_async_preview_pending();
            }
        },
        [completion_ctx,
         source,
         sync_completed,
         sync_draw_result,
         &snapshot,
         snapshot_layout_revision,
         snapshot_us,
         profiler_start,
         attempted_draw](const RendererPresentationSyncCompletion& completion) {
            if (sync_completed) {
                *sync_completed = true;
            }
            if (sync_draw_result) {
                *sync_draw_result = completion.draw;
            }
            finish_presented_draw(
                completion_ctx,
                source,
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
                completion.draw.frame_info_available
                    ? &completion.draw.frame_info
                    : nullptr);
        },
    };
}

} // namespace

bool RendererPresentCommandProcessor::draw_paused_frame(
    RendererPresentCommandContext& context,
    const char* reason) {
    const bool interactive_refresh =
        reason && (std::strcmp(reason, "macos-renderer-owned-refresh") == 0 ||
                   std::strcmp(reason, "request_frame_refresh") == 0);
    const bool decoded_preview_refresh =
        reason && std::strcmp(reason, "seek_frame_refresh") == 0;
    PresentDecision decision;
    bool has_frame = false;
    {
        std::lock_guard<std::mutex> lock(context.state_mutex);
        context.hooks.consume_pending_layout_locked();
        std::optional<PresentDecision> evaluated_decision;
        if (!decoded_preview_refresh && context.render_sink) {
            evaluated_decision = context.render_sink->evaluate();
        }
        const auto refresh_decision = context.tracks.paused_refresh_decision(
            context.history.snapshot(), evaluated_decision, decoded_preview_refresh);
        decision = refresh_decision.decision;
        has_frame = refresh_decision.has_frame;
    }
    if (!has_frame) {
        return false;
    }
    present_frame(context, decision);
    int ref = -1;
    {
        std::lock_guard<std::mutex> lock(context.state_mutex);
        context.tracks.filter_present_decision(decision);
        context.history.set(decision);
        ref = context.tracks.first_active_slot();
    }
    double pts = (ref >= 0 && decision.frames[ref].has_value())
                     ? decision.frames[ref]->pts_us / 1e6
                     : -1.0;
    if (interactive_refresh) {
        spdlog::debug(
            "[Renderer] draw_paused_frame({}): pts={:.3f}s", reason, pts);
    } else {
        spdlog::info("[Renderer] draw_paused_frame({}): pts={:.3f}s", reason, pts);
    }
    return true;
}

void RendererPresentCommandProcessor::present_frame(
    RendererPresentCommandContext& context,
    const PresentDecision& decision) {
    const auto profiler_start = std::chrono::steady_clock::now();
    RendererDrawSnapshot snapshot;
    uint64_t snapshot_layout_revision = 0;
    uint64_t snapshot_us = 0;
    {
        const auto snapshot_start = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(context.state_mutex);
        context.hooks.consume_pending_layout_locked();
        spdlog::debug("[present_frame] mode={}", context.layout.current_for_draw().mode);
        PresentDecision filtered_decision = decision;
        context.tracks.filter_present_decision(filtered_decision);
        RendererDrawSnapshotBuilder::update_track_geometry_from_decision(
            context.tracks, filtered_decision);
        snapshot = RendererDrawSnapshotBuilder::build(context.tracks,
                                                      context.layout,
                                                      context.surface,
                                                      filtered_decision);
        snapshot_layout_revision = context.layout.current_revision();
        snapshot_us = elapsed_us_since(snapshot_start);
    }
    const bool attempted_draw = present_decision_has_frame(snapshot.decision);
    const auto completion_ctx = completion_context(context);
    RendererPresentationSubmitRequest request(snapshot, context.metrics);
    request.source = "present_frame";
    request.offscreen = context.surface.offscreen();
    request.publish_swap_chain_after_sync_draw = true;
    request.poll_device_removed_label =
        context.surface.offscreen() ? "offscreen present" : nullptr;
    request.check_device_lost_after_draw = !context.surface.offscreen();
    request.overlay_hooks = context.hooks.overlay_hooks();
    request.should_abort_offscreen_publish =
        [&shutting_down = context.shutting_down]() {
            return shutting_down.load(std::memory_order_acquire);
        };
    request.async_completed =
        [completion_ctx,
         snapshot,
         snapshot_layout_revision,
         snapshot_us,
         profiler_start,
         attempted_draw](const RendererPresentationAsyncCompletion& completion) {
            finish_presented_draw(
                completion_ctx,
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
    context.presentation.submit_and_dispatch(
        std::move(request),
        dispatch_hooks(context,
                       completion_ctx,
                       "present_frame",
                       snapshot,
                       snapshot_layout_revision,
                       snapshot_us,
                       profiler_start,
                       attempted_draw,
                       &sync_completed,
                       &sync_draw_result));
    if (!sync_completed) {
        return;
    }
    const auto total_us = elapsed_us_since(profiler_start);
    static std::atomic<uint64_t> present_profiler_count{0};
    const auto count =
        present_profiler_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (profiler_enabled("VOIDPLAYER_MACOS_PROFILER") &&
        (total_us >= 8000 || sync_draw_result.backend_us >= 6000 ||
         count % 240 == 0)) {
        size_t active_tracks = 0;
        bool final_preview_drawn = false;
        {
            std::lock_guard<std::mutex> lock(context.state_mutex);
            active_tracks = context.tracks.count();
            final_preview_drawn = context.loop.preview_drawn();
        }
        spdlog::info(
            "[RendererProfiler] present_frame total_us={} snapshot_us={} backend_us={} "
            "attempted={} drew={} offscreen={} tracks={} layout_rev={} preview_drawn={}",
            total_us,
            snapshot_us,
            sync_draw_result.backend_us,
            attempted_draw,
            sync_draw_result.drew,
            context.surface.offscreen(),
            active_tracks,
            snapshot_layout_revision,
            final_preview_drawn);
    }
}

bool RendererPresentCommandProcessor::redraw_layout(
    RendererPresentCommandContext& context) {
    if (context.metrics
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
        std::lock_guard<std::mutex> lock(context.state_mutex);
        context.hooks.consume_pending_layout_locked();
        const auto decision_result =
            context.tracks.paused_layout_decision(context.history.snapshot());
        snapshot_layout_revision = context.layout.current_revision();
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
        context.history.set(decision);
        RendererDrawSnapshotBuilder::update_track_geometry_from_decision(
            context.tracks, decision);
        snapshot = RendererDrawSnapshotBuilder::build(context.tracks,
                                                      context.layout,
                                                      context.surface,
                                                      decision);
    }
    const bool attempted_draw = present_decision_has_frame(snapshot.decision);
    const auto completion_ctx = completion_context(context);
    RendererPresentationSubmitRequest request(snapshot, context.metrics);
    request.source = "viewport_composite";
    request.offscreen = context.surface.offscreen();
    request.wait_idle_after_sync_draw_label =
        context.surface.offscreen() ? nullptr : "viewport_composite";
    request.poll_device_removed_label =
        context.surface.offscreen() ? "offscreen redraw" : "viewport_composite";
    request.overlay_hooks = context.hooks.overlay_hooks();
    request.should_abort_offscreen_publish =
        [&shutting_down = context.shutting_down]() {
            return shutting_down.load(std::memory_order_acquire);
        };
    request.async_completed =
        [completion_ctx,
         snapshot,
         snapshot_layout_revision,
         snapshot_us,
         profiler_start,
         attempted_draw](const RendererPresentationAsyncCompletion& completion) {
            finish_presented_draw(completion_ctx,
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
    return context.presentation.submit_and_dispatch(
        std::move(request),
        dispatch_hooks(context,
                       completion_ctx,
                       "viewport_composite",
                       snapshot,
                       snapshot_layout_revision,
                       snapshot_us,
                       profiler_start,
                       attempted_draw,
                       nullptr,
                       nullptr));
}

} // namespace vr
