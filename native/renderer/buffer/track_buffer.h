#pragma once
#include "renderer/buffer/bidi_ring_buffer.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace vr {

enum class TrackState {
    Empty,
    Buffering,
    Ready,
    Flushing,
    Error
};

struct TrackBufferPushTiming {
    uint64_t lock_us = 0;
    uint64_t wait_us = 0;
    uint64_t push_us = 0;
    uint64_t ring_lock_us = 0;
    uint64_t ring_assign_us = 0;
    uint64_t ring_advance_us = 0;
    uint64_t ring_overwritten_cpu_bytes = 0;
    bool aborted = false;
    bool flushing = false;
};

class TrackBuffer {
public:
    explicit TrackBuffer(size_t forward_depth = 4, size_t backward_depth = 2);

    // Decode thread writes frames (blocks when buffer is full)
    void push_frame(TextureFrame frame, TrackBufferPushTiming* timing = nullptr);
    void set_state(TrackState state);

    // Abort: unblock any waiting push (called on shutdown/seek)
    void abort();

    // Reset after abort so the buffer can be reused.
    void reset();

    // Clear all buffered frames (used during seek to discard stale data)
    void clear_frames();

    // Render thread reads frames
    std::optional<TextureFrame> peek(int offset = 0) const;
    bool advance();
    bool retreat();
    bool can_retreat() const;
    size_t available_retreat_count() const;

    TrackState state() const;
    size_t total_count() const;
    size_t max_count() const;
    uint64_t estimated_cpu_bytes() const;
    int64_t last_presented_pts_us() const;
    void set_last_presented_pts_us(int64_t pts_us);

    // Preroll: minimum frames before Buffering → Ready transition
    bool has_preroll() const;
    size_t preroll_target() const { return preroll_target_; }

private:
    BidiRingBuffer ring_;
    std::atomic<TrackState> state_{TrackState::Empty};
    std::atomic<int64_t> last_presented_pts_us_{0};
    std::mutex push_mutex_;
    std::condition_variable push_cv_;
    std::atomic<bool> aborted_{false};
    size_t preroll_target_;
};

} // namespace vr
