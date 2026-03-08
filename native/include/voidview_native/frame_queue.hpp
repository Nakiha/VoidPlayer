#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace voidview {

/**
 * Thread-safe frame queue
 *
 * Used to pass frames between decoder and renderer threads.
 */
class FrameQueue {
public:
    explicit FrameQueue(size_t max_size = 4);
    ~FrameQueue();

    // Disable copy
    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    /**
     * Push frame (ownership transferred)
     * @param frame AVFrame pointer
     * @return True on success, false if queue full
     */
    bool push(AVFrame* frame);

    /**
     * Pop frame
     * @param timeout_ms Timeout in ms, -1 for infinite wait
     * @return AVFrame pointer, nullptr on timeout or abort
     */
    AVFrame* pop(int timeout_ms = -1);

    /**
     * Clear all frames
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

    // State queries
    bool is_full() const;
    bool is_empty() const;
    size_t size() const;
    size_t capacity() const;

private:
    std::queue<AVFrame*> queue_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    size_t max_size_;
    std::atomic<bool> aborted_{false};
};

} // namespace voidview
