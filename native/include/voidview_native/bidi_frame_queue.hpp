#pragma once

#include <deque>
#include <mutex>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace voidview {

/**
 * RAII wrapper for AVFrame
 *
 * Manages AVFrame lifecycle with move semantics.
 */
class FrameHolder {
public:
    explicit FrameHolder(AVFrame* frame = nullptr) : frame_(frame) {}
    ~FrameHolder() {
        if (frame_) {
            av_frame_free(&frame_);
        }
    }

    // Disable copy
    FrameHolder(const FrameHolder&) = delete;
    FrameHolder& operator=(const FrameHolder&) = delete;

    // Move constructor
    FrameHolder(FrameHolder&& other) noexcept : frame_(other.frame_) {
        other.frame_ = nullptr;
    }

    // Move assignment
    FrameHolder& operator=(FrameHolder&& other) noexcept {
        if (this != &other) {
            if (frame_) {
                av_frame_free(&frame_);
            }
            frame_ = other.frame_;
            other.frame_ = nullptr;
        }
        return *this;
    }

    bool is_valid() const { return frame_ != nullptr; }
    AVFrame* get() const { return frame_; }

    AVFrame* release() {
        AVFrame* f = frame_;
        frame_ = nullptr;
        return f;
    }

    void reset(AVFrame* new_frame = nullptr) {
        if (frame_) {
            av_frame_free(&frame_);
        }
        frame_ = new_frame;
    }

private:
    AVFrame* frame_ = nullptr;
};

/**
 * Bidirectional Frame Queue
 *
 * Supports both forward (future) and backward (history) frame navigation.
 * Used for frame-by-frame stepping in video players.
 *
 * Structure:
 *   [history] | current | [future]
 *    -4 -3 -2 -1 |  0  | +1 +2 +3 ...
 *
 * Thread-safe: all operations are protected by mutex.
 */
class BidiFrameQueue {
public:
    explicit BidiFrameQueue(size_t history_size = 4, size_t future_size = 12);
    ~BidiFrameQueue();

    // Disable copy
    BidiFrameQueue(const BidiFrameQueue&) = delete;
    BidiFrameQueue& operator=(const BidiFrameQueue&) = delete;

    // ==================== Frame Push ====================

    /**
     * Push frame to future queue (normal playback direction)
     * If future queue is full, discards the oldest frame (back)
     * @param frame AVFrame pointer, ownership transferred
     * @return True on success
     */
    bool push_future(AVFrame* frame);

    /**
     * Push frame to history queue (for prev_frame caching)
     * If history queue is full, discards the oldest frame (front)
     * @param frame AVFrame pointer, ownership transferred
     */
    void push_history(AVFrame* frame);

    // ==================== Current Frame ====================

    /**
     * Get current frame (does not transfer ownership)
     * @return AVFrame pointer, or nullptr if no current frame
     */
    AVFrame* current() const;

    /**
     * Check if current frame exists
     */
    bool has_current() const;

    /**
     * Get current frame PTS in milliseconds
     * @return PTS in ms, or -1 if no current frame
     */
    int64_t current_pts_ms() const;

    // ==================== Navigation ====================

    /**
     * Move to next frame (forward)
     * - Current frame moves to history
     * - First future frame becomes current
     * @return True on success, false if no future frames
     */
    bool move_next();

    /**
     * Move to previous frame (backward)
     * - Current frame moves to future (front)
     * - Last history frame becomes current
     * @return True on success, false if no history frames
     */
    bool move_prev();

    // ==================== State Query ====================

    size_t history_size() const;
    size_t future_size() const;
    size_t history_capacity() const { return max_history_; }
    size_t future_capacity() const { return max_future_; }

    bool has_history() const { return !history_.empty(); }
    bool has_future() const { return !future_.empty(); }

    /**
     * Get total frames in queue (history + current + future)
     */
    size_t total_size() const;

    /**
     * Check if queue is completely empty
     */
    bool is_empty() const;

    // ==================== Queue Management ====================

    /**
     * Clear all frames (history, current, future)
     */
    void clear();

    /**
     * Abort queue (wake all waiters)
     */
    void abort();

    /**
     * Reset abort state
     */
    void reset();

    /**
     * Take current frame (transfer ownership)
     * After this call, has_current() returns false
     * Note: Automatically saves a reference to history for prev_frame navigation
     * @return AVFrame pointer, caller owns it
     */
    AVFrame* take_current();

    /**
     * Take current frame without saving to history
     * Used by prev_frame to avoid circular history updates
     * @return AVFrame pointer, caller owns it
     */
    AVFrame* take_current_no_history();

    /**
     * Set current frame (take ownership)
     * @param frame AVFrame pointer
     */
    void set_current(AVFrame* frame);

private:
    mutable std::mutex mutex_;

    std::deque<FrameHolder> history_;  // Past frames (older at front)
    FrameHolder current_;               // Current frame
    std::deque<FrameHolder> future_;   // Future frames (newer at back)

    size_t max_history_;
    size_t max_future_;

    std::atomic<bool> aborted_{false};
};

} // namespace voidview
