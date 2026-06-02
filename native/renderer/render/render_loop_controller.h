#pragma once

#include <chrono>
#include <cstdint>

namespace vr {

class RenderLoopController {
public:
    void reset();
    void start(std::chrono::steady_clock::time_point now);

    bool should_apply_resize(std::chrono::steady_clock::time_point now) const;
    void mark_resize_applied(std::chrono::steady_clock::time_point now);

    bool should_emit_diagnostics(std::chrono::steady_clock::time_point now,
                                 int64_t pts_us,
                                 int64_t& pts_delta_us);

    std::chrono::microseconds frame_deadline_sleep(int64_t current_pts_us,
                                                   int64_t next_event_pts_us,
                                                   double speed,
                                                   int64_t max_sleep_us) const;

private:
    std::chrono::steady_clock::time_point last_resize_time_{};
    std::chrono::steady_clock::time_point diagnostic_time_{};
    int64_t diagnostic_last_pts_us_ = 0;
};

} // namespace vr
