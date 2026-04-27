#pragma once
#include "video_renderer/buffer/bidi_ring_buffer.h"
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace vr {

enum class TrackState {
    Empty,
    Buffering,
    Ready,
    Flushing,
    Error
};

class TrackBuffer {
public:
    explicit TrackBuffer(size_t forward_depth = 4, size_t backward_depth = 2);

    // Decode thread writes frames (blocks when buffer is full)
    void push_frame(TextureFrame frame);
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

    TrackState state() const;
    size_t total_count() const;
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
