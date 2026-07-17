#include "renderer/clock.h"
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
            static_cast<int64_t>(
                (pause_time_us_ - base_time_us_) * effective_speed_);
    }
    int64_t now = get_time_us();
    return base_pts_us_ +
        static_cast<int64_t>((now - base_time_us_) * effective_speed_);
}

void Clock::play() {
    std::lock_guard<std::mutex> lock(mutex_);
    int64_t now = get_time_us();
    base_time_us_ = now;
    base_pts_us_ = 0;
    requested_speed_ = 1.0;
    effective_speed_ = 1.0;
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
    if (new_speed <= 0) return;
    if (new_speed == requested_speed_ &&
        new_speed == effective_speed_) {
        return;
    }

    int64_t now = get_time_us();
    if (paused_) {
        now = pause_time_us_;
    }
    const int64_t current = base_pts_us_ +
        static_cast<int64_t>(
            (now - base_time_us_) * effective_speed_);
    base_pts_us_ = current;
    base_time_us_ = now;
    if (paused_) {
        pause_time_us_ = now;
    }
    requested_speed_ = new_speed;
    effective_speed_ = new_speed;
}

void Clock::set_effective_speed(double new_speed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (new_speed <= 0 || new_speed == effective_speed_) {
        return;
    }
    int64_t now = paused_ ? pause_time_us_ : get_time_us();
    const int64_t current = base_pts_us_ +
        static_cast<int64_t>(
            (now - base_time_us_) * effective_speed_);
    base_pts_us_ = current;
    base_time_us_ = now;
    if (paused_) {
        pause_time_us_ = now;
    }
    effective_speed_ = new_speed;
}

bool Clock::is_paused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

double Clock::speed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requested_speed_;
}

double Clock::effective_speed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return effective_speed_;
}

} // namespace vr
