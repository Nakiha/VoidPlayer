#include "renderer/buffer/track_buffer.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace vr {
namespace {

uint64_t elapsed_us_since(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}

} // namespace

TrackBuffer::TrackBuffer(size_t forward_depth, size_t backward_depth)
    : ring_(forward_depth, backward_depth)
    , preroll_target_(std::min(forward_depth, size_t(8)))
{}

void TrackBuffer::push_frame(TextureFrame frame, TrackBufferPushTiming* timing) {
    const auto lock_start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(push_mutex_);
    if (timing) {
        timing->lock_us = elapsed_us_since(lock_start);
    }
    const auto wait_start = std::chrono::steady_clock::now();
    push_cv_.wait(lock, [this] {
        return ring_.total_count() < ring_.max_count() || aborted_.load();
    });
    if (timing) {
        timing->wait_us = elapsed_us_since(wait_start);
    }
    if (aborted_.load()) {
        if (timing) {
            timing->aborted = true;
        }
        return;
    }
    // Discard frames pushed during Flushing (seek transition race)
    if (state_.load(std::memory_order_acquire) == TrackState::Flushing) {
        if (timing) {
            timing->flushing = true;
        }
        return;
    }

    const auto push_start = std::chrono::steady_clock::now();
    BidiRingBuffer::PushTiming ring_timing;
    ring_.push(std::move(frame), &ring_timing);
    if (timing) {
        timing->push_us = elapsed_us_since(push_start);
        timing->ring_lock_us = ring_timing.lock_us;
        timing->ring_assign_us = ring_timing.assign_us;
        timing->ring_advance_us = ring_timing.advance_us;
        timing->ring_overwritten_cpu_bytes =
            ring_timing.overwritten_cpu_bytes;
    }
}

void TrackBuffer::set_state(TrackState state) {
    state_.store(state, std::memory_order_release);
    // Wake decode thread if entering Flushing so it can discard in-progress frames
    if (state == TrackState::Flushing) {
        push_cv_.notify_one();
    }
}

void TrackBuffer::abort() {
    aborted_.store(true);
    push_cv_.notify_all();
}

void TrackBuffer::reset() {
    aborted_.store(false);
    ring_.clear();
    last_presented_pts_us_.store(0, std::memory_order_release);
    state_.store(TrackState::Empty, std::memory_order_release);
    push_cv_.notify_all();
}

void TrackBuffer::clear_frames() {
    ring_.clear();
    // Notify decode thread that slots have been freed
    push_cv_.notify_one();
}

std::optional<TextureFrame> TrackBuffer::peek(int offset) const {
    return ring_.peek(offset);
}

bool TrackBuffer::advance() {
    auto current = ring_.peek(0);
    if (!current.has_value()) return false;

    int64_t pts = current->pts_us;

    if (!ring_.advance()) return false;

    last_presented_pts_us_.store(pts, std::memory_order_release);

    // Notify decode thread that a slot has been freed
    push_cv_.notify_one();
    return true;
}

bool TrackBuffer::advance_history_cursor() {
    if (!ring_.advance()) return false;

    // The predecessor exists solely to seed bidirectional stepping. Keep
    // last_presented_pts_us_ tied to an actual presentation decision.
    push_cv_.notify_one();
    return true;
}

bool TrackBuffer::retreat() {
    return ring_.retreat();
}

bool TrackBuffer::can_retreat() const {
    return ring_.can_retreat();
}

size_t TrackBuffer::available_retreat_count() const {
    return ring_.available_retreat_count();
}

TrackState TrackBuffer::state() const {
    return state_.load(std::memory_order_acquire);
}

size_t TrackBuffer::total_count() const {
    return ring_.total_count();
}

size_t TrackBuffer::max_count() const {
    return ring_.max_count();
}

uint64_t TrackBuffer::estimated_cpu_bytes() const {
    return ring_.estimated_cpu_bytes();
}

int64_t TrackBuffer::last_presented_pts_us() const {
    return last_presented_pts_us_.load(std::memory_order_acquire);
}

void TrackBuffer::set_last_presented_pts_us(int64_t pts_us) {
    last_presented_pts_us_.store(pts_us, std::memory_order_release);
}

bool TrackBuffer::has_preroll() const {
    return ring_.total_count() >= preroll_target_;
}

} // namespace vr
