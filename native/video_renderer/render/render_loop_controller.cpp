#include "video_renderer/render/render_loop_controller.h"

#include <algorithm>

namespace vr {
namespace {

constexpr auto kResizeDebounceInterval = std::chrono::milliseconds(33);
constexpr auto kDiagnosticInterval = std::chrono::seconds(2);

} // namespace

void RenderLoopController::reset() {
    last_resize_time_ = {};
    diagnostic_time_ = {};
    diagnostic_last_pts_us_ = 0;
}

void RenderLoopController::start(std::chrono::steady_clock::time_point now) {
    diagnostic_time_ = now;
    diagnostic_last_pts_us_ = 0;
}

bool RenderLoopController::should_apply_resize(std::chrono::steady_clock::time_point now) const {
    return now - last_resize_time_ >= kResizeDebounceInterval;
}

void RenderLoopController::mark_resize_applied(std::chrono::steady_clock::time_point now) {
    last_resize_time_ = now;
}

bool RenderLoopController::should_emit_diagnostics(std::chrono::steady_clock::time_point now,
                                                   int64_t pts_us,
                                                   int64_t& pts_delta_us) {
    if (now - diagnostic_time_ < kDiagnosticInterval) {
        return false;
    }
    diagnostic_time_ = now;
    pts_delta_us = pts_us - diagnostic_last_pts_us_;
    diagnostic_last_pts_us_ = pts_us;
    return true;
}

std::chrono::microseconds RenderLoopController::frame_deadline_sleep(
    int64_t current_pts_us,
    int64_t next_event_pts_us,
    double speed,
    int64_t max_sleep_us) const {
    if (speed <= 0.0 || max_sleep_us <= 0 || next_event_pts_us <= current_pts_us) {
        return std::chrono::microseconds(0);
    }
    const int64_t pts_delta_us = next_event_pts_us - current_pts_us;
    int64_t sleep_us = static_cast<int64_t>(pts_delta_us / speed);
    if (sleep_us <= 0) {
        return std::chrono::microseconds(0);
    }
    sleep_us = std::min(sleep_us, max_sleep_us);
    return std::chrono::microseconds(sleep_us);
}

} // namespace vr
