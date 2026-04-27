#include "video_renderer/buffer/packet_queue.h"
#include <spdlog/spdlog.h>

namespace vr {

void PacketQueue::packet_deleter(AVPacket* pkt) {
    if (pkt) {
        av_packet_free(&pkt);
    }
}

PacketQueue::PacketQueue(size_t capacity)
    : capacity_(capacity)
{}

PacketQueue::~PacketQueue() {
    flush();
}

bool PacketQueue::push(AVPacket* pkt) {
    std::unique_lock<std::mutex> lock(mutex_);
    not_full_.wait(lock, [this]() { return queue_.size() < capacity_ || aborted_; });
    if (aborted_) return false;  // caller retains ownership of pkt
    auto ptr = PacketPtr(pkt, &packet_deleter);
    queue_.push(std::move(ptr));
    not_empty_.notify_one();
    return true;
}

AVPacket* PacketQueue::pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [this]() { return !queue_.empty() || aborted_; });
    if (aborted_ && queue_.empty()) return nullptr;
    if (queue_.empty()) return nullptr;
    auto ptr = std::move(queue_.front());
    queue_.pop();
    not_full_.notify_one();
    return ptr.release();
}

AVPacket* PacketQueue::try_pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (aborted_ || queue_.empty()) return nullptr;
    auto ptr = std::move(queue_.front());
    queue_.pop();
    not_full_.notify_one();
    return ptr.release();
}

void PacketQueue::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
        queue_.pop();
    }
    not_full_.notify_all();
}

void PacketQueue::abort() {
    std::lock_guard<std::mutex> lock(mutex_);
    aborted_ = true;
    not_full_.notify_all();
    not_empty_.notify_all();
}

void PacketQueue::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
        queue_.pop();
    }
    aborted_ = false;
    not_full_.notify_all();
}

size_t PacketQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool PacketQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

bool PacketQueue::is_aborted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aborted_;
}

void PacketQueue::signal_eof() {
    eof_.store(true, std::memory_order_release);
}

void PacketQueue::clear_eof() {
    eof_.store(false, std::memory_order_release);
}

bool PacketQueue::is_eof() const {
    return eof_.load(std::memory_order_acquire);
}

} // namespace vr
