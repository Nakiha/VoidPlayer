#pragma once

#include "renderer/playback/playback_pacing_controller.h"
#include "renderer/render/render_loop_controller.h"
#include "renderer/render/presentation_scheduler.h"
#include "renderer/render/renderer_preview_state.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

namespace vr {

struct RendererLoopResizeDecision {
    bool should_apply = false;
    int width = 0;
    int height = 0;
};

struct RendererLoopDiagnosticDecision {
    bool should_emit = false;
    int64_t pts_delta_us = 0;
};

// Lock contract:
// - Owns render-thread lifetime state, resize debounce state, and the loop
//   timing/diagnostic/presentation scheduler state.
// - Does not take renderer locks and does not know tracks/layout/presentation
//   internals beyond ticking a RenderSink snapshot.
// - Callers coordinate lifecycle/state locks before starting or stopping.
class RendererLoopDriver {
public:
    template <typename Fn, typename... Args>
    void start_thread(Fn&& fn, Args&&... args) {
        thread_ = std::thread(std::forward<Fn>(fn), std::forward<Args>(args)...);
    }

    bool thread_joinable() const;
    void join_thread();

    bool running() const;
    void set_running(bool running);

    void reset_timing();
    void start_loop(std::chrono::steady_clock::time_point now);

    void request_resize(int width, int height);
    RendererLoopResizeDecision take_resize_decision(
        std::chrono::steady_clock::time_point now);
    void clear_pending_resize();
    void reset_playback_pacing();
    PlaybackPacingDecision evaluate_playback_pacing(
        const PlaybackPacingInput& input);
    PlaybackPacingDiagnostics playback_pacing_diagnostics() const;
    void reset_preview_state();
    void force_preview_redraw();
    void mark_preview_presented(bool drawn = true);
    void clear_preview_pending();
    bool preview_drawn() const;
    bool preview_pending() const;
    bool begin_preview_draw_if_needed();
    void mark_async_preview_pending();
    RendererLoopDiagnosticDecision take_diagnostic_decision(
        std::chrono::steady_clock::time_point now,
        int64_t pts_us);
    void reset_presentation_scheduler();
    PresentationSchedulerTick tick_presentation(RenderSink& sink);
    void commit_presented(const PresentDecision& decision);
    std::chrono::microseconds frame_deadline_sleep(int64_t current_pts_us,
                                                   int64_t next_event_pts_us,
                                                   double speed,
                                                   int64_t max_sleep_us) const;

private:
    std::pair<int, int> take_pending_resize();
    void requeue_resize_if_empty(int width, int height);

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<int> pending_width_{0};
    std::atomic<int> pending_height_{0};
    PlaybackPacingController playback_pacing_;
    RendererPreviewState preview_;
    RenderLoopController controller_;
    PresentationScheduler presentation_scheduler_;
};

} // namespace vr
