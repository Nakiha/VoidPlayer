#pragma once

#include "renderer/metrics/presentation_metrics_store.h"
#include "renderer/playback/renderer_timeline_controller.h"
#include "renderer/render/renderer_loop_driver.h"
#include "renderer/render/renderer_present_command.h"
#include "renderer/render/renderer_present_history.h"
#include "renderer/render/renderer_presentation_controller.h"
#include "renderer/layout/renderer_layout_state.h"
#include "renderer/sync/render_sink.h"
#include "renderer/track/renderer_track_controller.h"

#include <functional>
#include <memory>
#include <mutex>

namespace vr {

struct RendererRenderLoopCommandHooks {
    // Requires state_mutex held.
    std::function<void(const char* operation)> enter_terminal_device_lost_locked;
    // Receive the already-held state lock because they may temporarily
    // unlock/relock during long operations.
    std::function<bool(std::unique_lock<std::mutex>& state_lock)>
        apply_deferred_paused_hevc_seek_locked;
    std::function<bool(std::unique_lock<std::mutex>& state_lock)>
        apply_loop_range_locked;
    // Requires state_mutex held.
    std::function<void()> consume_pending_layout_locked;
    std::function<void(int width, int height)> do_resize;
    std::function<RendererPresentCommandContext()> present_command_context;
    std::function<void(bool paused)> set_decode_paused_for_all_tracks;
    // Requires state_mutex held.
    std::function<void()> mark_paused_hevc_seek_preview_drawn_locked;
    std::function<void(const PresentDecision& decision)>
        emit_seek_preview_presented_events;
    std::function<void(bool force)> emit_playback_clock_event;
    // Requires state_mutex held.
    std::function<bool(int64_t end_pts_us)> settle_eof_locked;
};

struct RendererRenderLoopCommandContext {
    std::mutex& state_mutex;
    std::mutex& lifecycle_mutex;
    RendererLoopDriver& loop;
    RendererTimelineController& timeline;
    RendererTrackController& tracks;
    RendererPresentationController& presentation;
    RendererPresentHistory& history;
    RendererLayoutState& layout;
    PresentationMetricsStore& metrics;
    std::unique_ptr<RenderSink>& render_sink;
    RendererRenderLoopCommandHooks hooks;
};

class RendererRenderLoopCommandProcessor {
public:
    static void run_body(RendererRenderLoopCommandContext& context);
};

} // namespace vr
