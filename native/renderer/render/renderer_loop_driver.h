#pragma once

#include "renderer/render/render_loop_controller.h"
#include "renderer/render/presentation_scheduler.h"
#include "renderer/render/renderer_preview_state.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <utility>

namespace vr {

struct RendererLoopPrerollDecision {
    bool clock_paused = false;
    bool force_preview_redraw = false;
    bool pause_clock = false;
    bool resume_clock = false;
    bool resume_decode = false;
    bool log_transition_complete = false;
    bool log_preroll_pending = false;
    bool log_preroll_complete = false;
    double preroll_complete_pts_s = -1.0;
};

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
    void reset_preroll_state();
    RendererLoopPrerollDecision evaluate_preroll(bool playing,
                                                 bool clock_paused,
                                                 bool any_buffering,
                                                 int64_t current_pts_us);
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
    bool was_buffering_ = false;
    RendererPreviewState preview_;
    RenderLoopController controller_;
    PresentationScheduler presentation_scheduler_;
};

} // namespace vr
