#include "video_renderer/clock.h"
#include <chrono>

namespace vr {

static int64_t real_time_us() {
    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch());
    return us.count();
}

Clock::Clock(TimeSource time_source)
    : time_source_(time_source ? std::move(time_source) : real_time_us)
{}

int64_t Clock::get_time_us() const {
    return time_source_();
}

int64_t Clock::current_pts_us() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_) {
        return base_pts_us_ +
            static_cast<int64_t>((pause_time_us_ - base_time_us_) * speed_);
    }
    int64_t now = get_time_us();
    return base_pts_us_ + static_cast<int64_t>((now - base_time_us_) * speed_);
}

void Clock::play() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = get_time_us();
    base_time_us_ = now;
    base_pts_us_ = 0;
    speed_ = 1.0;
    paused_ = false;
}

void Clock::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_) return;
    pause_time_us_ = get_time_us();
    paused_ = true;
}

void Clock::resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!paused_) return;
    int64_t now = get_time_us();
    base_time_us_ += (now - pause_time_us_);
    paused_ = false;
}

void Clock::seek(int64_t target_pts_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = get_time_us();
    base_pts_us_ = target_pts_us;
    base_time_us_ = now;
    if (paused_) {
        pause_time_us_ = now;
    }
}

void Clock::set_speed(double new_speed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (new_speed == speed_) return;
    if (new_speed <= 0) return;

    // Preserve current PTS: current = base_pts + (now - base_time) * old_speed
    // new_base_time = now - (current - base_pts) / new_speed
    int64_t now = get_time_us();
    if (paused_) {
        now = pause_time_us_;
    }
    int64_t current = base_pts_us_ + static_cast<int64_t>((now - base_time_us_) * speed_);
    base_time_us_ = now - static_cast<int64_t>((current - base_pts_us_) / new_speed);
    speed_ = new_speed;
}

bool Clock::is_paused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

double Clock::speed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return speed_;
}

} // namespace vr
