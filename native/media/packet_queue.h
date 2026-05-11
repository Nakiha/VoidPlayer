#pragma once
#include <deque>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <cstdint>
#include <atomic>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace vr {

enum class PacketPopStatus {
    Packet,
    Flushed,
    Eof,
    Aborted,
    Empty,
};

struct PacketPopResult {
    PacketPopStatus status = PacketPopStatus::Empty;
    AVPacket* packet = nullptr;
};

class PacketQueue {
public:
    explicit PacketQueue(size_t capacity = 100);
    ~PacketQueue();

    // Push a packet (takes ownership). Blocks if full. Returns false if aborted.
    bool push(AVPacket* pkt);

    // Push a packet without blocking. Takes ownership only on success.
    bool try_push(AVPacket* pkt);

    // Pop a packet (caller takes ownership). Blocks until packet/state change.
    PacketPopResult pop();

    // Non-blocking pop with explicit empty/EOF/abort status.
    PacketPopResult try_pop();

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
    uint64_t estimated_bytes() const;
    bool empty() const;
    bool is_aborted() const;

private:
    static void packet_deleter(AVPacket* pkt);
    using PacketPtr = std::unique_ptr<AVPacket, decltype(&packet_deleter)>;

    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<PacketPtr> queue_;
    size_t capacity_;
    bool aborted_ = false;
    bool flushed_ = false;
    std::atomic<bool> eof_{false};
};

} // namespace vr
