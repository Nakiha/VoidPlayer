#pragma once
#include <cstdint>
#include <mutex>
#include <functional>
#include <chrono>

namespace vr {

class Clock {
public:
    // Injectable time source for testing (defaults to real steady_clock)
    using TimeSource = std::function<int64_t()>;

    explicit Clock(TimeSource time_source = nullptr);

    // Query current PTS based on wall-clock time
    int64_t current_pts_us() const;

    // Operations
    void play();                        // base_time_us = now, paused = false
    void pause();                       // pause_time_us = now, paused = true
    void resume();                      // base_time_us += (now - pause_time_us)
    void seek(int64_t target_pts_us);   // base_pts_us = target, base_time_us = now
    void set_speed(double new_speed);   // preserve current_pts_us, adjust base_time_us

    // State queries
    bool is_paused() const;
    double speed() const;

private:
    int64_t get_time_us() const;

    mutable std::mutex mutex_;
    TimeSource time_source_;
    int64_t base_time_us_ = 0;
    int64_t base_pts_us_ = 0;
    int64_t pause_time_us_ = 0;
    double speed_ = 1.0;
    bool paused_ = true;
};

} // namespace vr
