#pragma once

#include "renderer/metrics/presentation_metrics_store.h"
#include "renderer/render/renderer_loop_driver.h"
#include "renderer/render/renderer_present_history.h"
#include "renderer/render/renderer_presentation_controller.h"
#include "renderer/render/renderer_surface_state.h"
#include "renderer/sync/render_sink.h"
#include "renderer/layout/renderer_layout_state.h"
#include "renderer/track/renderer_track_controller.h"

#include <atomic>
#include <functional>
#include <mutex>

namespace vr {

struct RendererPresentCommandHooks {
    // Requires state_mutex held.
    std::function<void()> consume_pending_layout_locked;
    // Must not take the renderer state lock.
    std::function<RendererPresentationOverlayHooks()> overlay_hooks;
    // Requires state_mutex held.
    std::function<void(const char* operation)> enter_terminal_device_lost_locked;
    // Called with state_mutex held.
    std::function<bool()> playback_inactive_or_paused;
};

struct RendererPresentCommandContext {
    std::mutex& state_mutex;
    RendererTrackController& tracks;
    RendererLayoutState& layout;
    RendererSurfaceState& surface;
    RendererPresentationController& presentation;
    RenderSink* render_sink = nullptr;
    RendererPresentHistory& history;
    RendererLoopDriver& loop;
    PresentationMetricsStore& metrics;
    std::atomic<bool>& shutting_down;
    RendererPresentCommandHooks hooks;
};

class RendererPresentCommandProcessor {
public:
    static bool draw_paused_frame(RendererPresentCommandContext& context,
                                  const char* reason);
    static RendererFrameRefreshResult redraw_layout(
        RendererPresentCommandContext& context);
    static void present_frame(RendererPresentCommandContext& context,
                              const PresentDecision& decision);
};

} // namespace vr
