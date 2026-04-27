#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>
#include <optional>

namespace vr {

enum class SeekType {
    Keyframe,
    Exact
};

struct SeekRequest {
    int64_t target_pts_us = 0;
    SeekType type = SeekType::Keyframe;
};

/// Thread-safe seek request coordinator.
/// The Renderer submits requests, the DemuxThread polls and executes them.
class SeekController {
public:
    SeekController();

    /// Submit a seek request (called from any thread, typically Renderer).
    void request_seek(int64_t target_pts_us, SeekType type);

    /// Check if a seek request is pending (lock-free, for polling).
    bool has_pending_seek() const;

    /// Atomically take the pending request: returns the request and clears the flag.
    /// Returns std::nullopt if no request is pending.
    std::optional<SeekRequest> take_pending();

private:
    SeekRequest pending_;
    std::atomic<bool> has_pending_{false};
    mutable std::mutex pending_mutex_;
};

} // namespace vr
