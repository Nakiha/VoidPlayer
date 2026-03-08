#include "voidview_native/frame_queue.hpp"

namespace voidview {

FrameQueue::FrameQueue(size_t max_size)
    : max_size_(max_size) {
}

FrameQueue::~FrameQueue() {
    clear();
}

bool FrameQueue::push(AVFrame* frame) {
    if (!frame) return false;

    std::unique_lock<std::mutex> lock(mutex_);

    // Wait for space if queue is full
    not_full_.wait(lock, [this] {
        return queue_.size() < max_size_ || aborted_.load();
    });

    if (aborted_.load()) {
        return false;
    }

    queue_.push(frame);
    not_empty_.notify_one();
    return true;
}

AVFrame* FrameQueue::pop(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);

    if (timeout_ms < 0) {
        // Infinite wait
        not_empty_.wait(lock, [this] {
            return !queue_.empty() || aborted_.load();
        });
    } else {
        // Timed wait
        if (!not_empty_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                 [this] {
                                     return !queue_.empty() || aborted_.load();
                                 })) {
            return nullptr;  // Timeout
        }
    }

    if (aborted_.load() || queue_.empty()) {
        return nullptr;
    }

    AVFrame* frame = queue_.front();
    queue_.pop();
    not_full_.notify_one();
    return frame;
}

void FrameQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
        AVFrame* frame = queue_.front();
        queue_.pop();
        if (frame) {
            av_frame_free(&frame);
        }
    }
}

void FrameQueue::abort() {
    aborted_.store(true);
    not_empty_.notify_all();
    not_full_.notify_all();
}

void FrameQueue::reset() {
    aborted_.store(false);
}

bool FrameQueue::is_full() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() >= max_size_;
}

bool FrameQueue::is_empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

size_t FrameQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

size_t FrameQueue::capacity() const {
    return max_size_;
}

} // namespace voidview
