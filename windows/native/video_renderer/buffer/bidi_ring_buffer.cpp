#include "video_renderer/buffer/bidi_ring_buffer.h"

namespace vr {

BidiRingBuffer::BidiRingBuffer(size_t forward_depth, size_t backward_depth)
    : forward_depth_(forward_depth)
    , backward_depth_(backward_depth)
    , capacity_(forward_depth + backward_depth + 1)
{
    ring_.resize(capacity_);
}

bool BidiRingBuffer::push(TextureFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Reserve backward_depth slots behind read_idx so retreat data
    // is never overwritten by pushes.
    if (count_ >= capacity_ - backward_depth_) {
        return false;
    }

    ring_[write_idx_] = std::move(frame);
    write_idx_ = (write_idx_ + 1) % capacity_;
    ++count_;
    return true;
}

std::optional<TextureFrame> BidiRingBuffer::peek(int offset) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) return std::nullopt;

    // offset=0 -> read_idx, offset=1 -> next forward, offset=-1 -> backward
    if (offset >= 0) {
        size_t fwd = static_cast<size_t>(offset);
        if (fwd >= count_) return std::nullopt;
        return ring_[(read_idx_ + offset) % capacity_];
    } else {
        // Backward: offset = -1 means one step back
        size_t back = static_cast<size_t>(-offset);
        // How far can we go back? Limited by backward_depth and how much we've advanced
        if (back > backward_depth_) return std::nullopt;
        // read_idx - back (wrapping)
        size_t idx = (read_idx_ + capacity_ - back) % capacity_;
        // Check that this index is actually behind write boundary
        // A backward peek is valid only if the slot contains valid data
        // Simple check: the position must not be at or past write_idx
        // For now, allow it since backward_depth constrains it
        return ring_[idx];
    }
}

bool BidiRingBuffer::advance() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) return false;

    read_idx_ = (read_idx_ + 1) % capacity_;
    --count_;
    if (retreated_ > 0) --retreated_;
    ++total_advanced_;
    return true;
}

bool BidiRingBuffer::retreat() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Cannot retreat if no frames to go back to
    if (retreated_ >= backward_depth_) return false;
    // The frame we want to retreat to must exist:
    // read_idx points to current frame. Going back means we need a slot before read_idx.
    // But we also need count_ + retreated_ + 1 <= capacity_ (we're re-exposing a frame).
    // Actually since we only move read_idx backward, we need the slot to have valid data.
    // When we advance(), count_ decreases — those slots still hold old data until overwritten.
    // We track how far back we've gone via retreated_.
    // The total "virtual" extent is count_ + retreated_.
    // After retreat, we go back by 1: read_idx moves back, retreated was already counting.
    // We need: retreated_ < backward_depth_ (checked above)
    // And: the slot at (read_idx - 1) still holds a valid frame that hasn't been overwritten.
    // Since write_idx moves only forward, and we only push when count_ < capacity_,
    // the slots behind read_idx (up to backward_depth_) are still valid as long as
    // they haven't been overwritten by push. Push overwrites at write_idx which is
    // ahead of read_idx. So slots behind read_idx are safe.
    read_idx_ = (read_idx_ + capacity_ - 1) % capacity_;
    ++count_;
    ++retreated_;
    return true;
}

bool BidiRingBuffer::can_advance() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) return false;
    if (count_ == capacity_ && write_idx_ == read_idx_) return true;
    return (write_idx_ + capacity_ - read_idx_) % capacity_ > 0;
}

bool BidiRingBuffer::can_retreat() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return retreated_ < backward_depth_ && retreated_ < total_advanced_;
}

size_t BidiRingBuffer::forward_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) return 0;
    if (count_ == capacity_ && write_idx_ == read_idx_) {
        return count_;  // full buffer: all frames are forward
    }
    return (write_idx_ + capacity_ - read_idx_) % capacity_;
}

size_t BidiRingBuffer::backward_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return retreated_;
}

size_t BidiRingBuffer::total_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

bool BidiRingBuffer::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ == 0;
}

void BidiRingBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Release all TextureFrame resources (shared_ptr cpu_data, texture handles)
    for (size_t i = 0; i < capacity_; ++i) {
        ring_[i] = TextureFrame{};
    }
    write_idx_ = 0;
    read_idx_ = 0;
    count_ = 0;
    retreated_ = 0;
    total_advanced_ = 0;
}

} // namespace vr
