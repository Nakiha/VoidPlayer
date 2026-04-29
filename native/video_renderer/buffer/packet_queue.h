#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <cstdint>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace vr {

class PacketQueue {
public:
    explicit PacketQueue(size_t capacity = 100);
    ~PacketQueue();

    // Push a packet (takes ownership). Blocks if full. Returns false if aborted.
    bool push(AVPacket* pkt);

    // Pop a packet (caller takes ownership). Blocks if empty. Returns nullptr if aborted.
    AVPacket* pop();

    // Non-blocking pop. Returns nullptr if empty or aborted.
    AVPacket* try_pop();

    // Flush: discard queued packets and clear EOF state.
    void flush();

    // Abort: unblock all waiters
    void abort();

    // Reset after abort (reuse)
    void reset();

    // EOF signal (producer sets, consumer reads)
    void signal_eof();
    void clear_eof();
    bool is_eof() const;

    // State
    size_t size() const;
    bool empty() const;
    bool is_aborted() const;

private:
    static void packet_deleter(AVPacket* pkt);
    using PacketPtr = std::unique_ptr<AVPacket, decltype(&packet_deleter)>;

    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::queue<PacketPtr> queue_;
    size_t capacity_;
    bool aborted_ = false;
    std::atomic<bool> eof_{false};
};

} // namespace vr
