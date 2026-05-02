#include "video_renderer/buffer/track_buffer.h"
#include <spdlog/spdlog.h>

namespace vr {

TrackBuffer::TrackBuffer(size_t forward_depth, size_t backward_depth)
    : ring_(forward_depth, backward_depth)
    , preroll_target_(std::min(forward_depth, size_t(8)))
{}

void TrackBuffer::push_frame(TextureFrame frame) {
    std::unique_lock<std::mutex> lock(push_mutex_);
    push_cv_.wait(lock, [this] {
        return ring_.total_count() < ring_.max_count() || aborted_.load();
    });
    if (aborted_.load()) return;
    // Discard frames pushed during Flushing (seek transition race)
    if (state_.load(std::memory_order_acquire) == TrackState::Flushing) return;

    ring_.push(std::move(frame));
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

bool TrackBuffer::retreat() {
    return ring_.retreat();
}

bool TrackBuffer::can_retreat() const {
    return ring_.can_retreat();
}

TrackState TrackBuffer::state() const {
    return state_.load(std::memory_order_acquire);
}

size_t TrackBuffer::total_count() const {
    return ring_.total_count();
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
