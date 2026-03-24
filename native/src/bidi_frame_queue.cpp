#include "voidview_native/bidi_frame_queue.hpp"
#include "voidview_native/logger.hpp"

namespace voidview {

BidiFrameQueue::BidiFrameQueue(size_t history_size, size_t future_size)
    : max_history_(history_size)
    , max_future_(future_size)
{
}

BidiFrameQueue::~BidiFrameQueue() {
    clear();
}

bool BidiFrameQueue::push_future(AVFrame* frame) {
    if (!frame) return false;

    std::lock_guard<std::mutex> lock(mutex_);

    // If future is full, discard the oldest (back) frame
    if (future_.size() >= max_future_) {
        future_.pop_back();
        VV_TRACE("BidiFrameQueue::push_future: discarded oldest future frame (queue full)");
    }

    future_.emplace_back(frame);
    VV_TRACE("BidiFrameQueue::push_future: frame pushed, future_size={}", future_.size());
    return true;
}

void BidiFrameQueue::push_history(AVFrame* frame) {
    if (!frame) return;

    std::lock_guard<std::mutex> lock(mutex_);

    // If history is full, discard the oldest (front) frame
    if (history_.size() >= max_history_) {
        history_.pop_front();
        VV_TRACE("BidiFrameQueue::push_history: discarded oldest history frame (queue full)");
    }

    history_.emplace_back(frame);
    VV_TRACE("BidiFrameQueue::push_history: frame pushed, history_size={}", history_.size());
}

AVFrame* BidiFrameQueue::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_.get();
}

bool BidiFrameQueue::has_current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_.is_valid();
}

int64_t BidiFrameQueue::current_pts_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_.is_valid()) {
        return -1;
    }

    AVFrame* frame = current_.get();
    if (frame->pts != AV_NOPTS_VALUE) {
        // Assuming time_base is 1/1000 (milliseconds)
        return frame->pts;
    }
    return -1;
}

bool BidiFrameQueue::move_next() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (future_.empty()) {
        VV_TRACE("BidiFrameQueue::move_next: no future frames");
        return false;
    }

    // Move current to history (if exists)
    if (current_.is_valid()) {
        // If history is full, discard oldest
        if (history_.size() >= max_history_) {
            history_.pop_front();
        }
        history_.push_back(std::move(current_));
    }

    // Take first future frame as current
    current_ = std::move(future_.front());
    future_.pop_front();

    VV_TRACE("BidiFrameQueue::move_next: moved to next, history={}, future={}",
             history_.size(), future_.size());
    return true;
}

bool BidiFrameQueue::move_prev() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (history_.empty()) {
        VV_TRACE("BidiFrameQueue::move_prev: no history frames");
        return false;
    }

    // Move current to future front (if exists)
    if (current_.is_valid()) {
        // If future is full, discard oldest (back)
        if (future_.size() >= max_future_) {
            future_.pop_back();
        }
        future_.push_front(std::move(current_));
    }

    // Take last history frame as current
    AVFrame* history_frame = history_.back().get();
    int64_t history_pts = history_frame ? history_frame->pts : -1;
    VV_TRACE("BidiFrameQueue::move_prev: taking frame from history, pts={}", history_pts);

    current_ = std::move(history_.back());
    history_.pop_back();

    VV_TRACE("BidiFrameQueue::move_prev: moved to prev, history={}, future={}",
             history_.size(), future_.size());
    return true;
}

size_t BidiFrameQueue::history_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return history_.size();
}

size_t BidiFrameQueue::future_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return future_.size();
}

size_t BidiFrameQueue::total_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t total = history_.size() + future_.size();
    if (current_.is_valid()) {
        total += 1;
    }
    return total;
}

bool BidiFrameQueue::is_empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return history_.empty() && !current_.is_valid() && future_.empty();
}

void BidiFrameQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    history_.clear();
    current_.reset();
    future_.clear();
    VV_TRACE("BidiFrameQueue::clear: queue cleared");
}

void BidiFrameQueue::abort() {
    aborted_.store(true);
}

void BidiFrameQueue::reset() {
    aborted_.store(false);
}

AVFrame* BidiFrameQueue::take_current() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!current_.is_valid()) {
        return nullptr;
    }

    // 先保存当前帧到 history（用于 prev_frame）
    // 使用 av_frame_ref() 创建引用（增加引用计数）
    if (history_.size() >= max_history_) {
        history_.pop_front();
    }

    AVFrame* history_frame = av_frame_alloc();
    if (history_frame) {
        int ret = av_frame_ref(history_frame, current_.get());
        if (ret == 0) {
            history_.emplace_back(history_frame);
            VV_TRACE("BidiFrameQueue::take_current: saved frame to history, pts={}, history={}",
                     history_frame->pts, history_.size());
        } else {
            av_frame_free(&history_frame);
            VV_WARN("BidiFrameQueue::take_current: failed to ref frame, ret={}", ret);
        }
    }

    // 返回原始帧（转移所有权）
    return current_.release();
}

AVFrame* BidiFrameQueue::take_current_no_history() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!current_.is_valid()) {
        return nullptr;
    }

    // 直接返回当前帧，不保存到 history
    return current_.release();
}

void BidiFrameQueue::set_current(AVFrame* frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_.reset(frame);
}

} // namespace voidview
