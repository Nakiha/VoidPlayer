#pragma once

#include "video_renderer/sync/render_sink.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace vr {

class TrackPerfBaselineTracker {
public:
    using Clock = std::chrono::steady_clock;

    void reset(Clock::time_point start_time = Clock::time_point{});
    Clock::time_point start_time() const { return start_time_; }

    double elapsed_seconds(Clock::time_point now) const;
    bool should_rotate(double elapsed_seconds) const;
    void rotate_timer(Clock::time_point start_time);

    uint64_t baseline_frames(size_t slot) const;
    void update_baseline_frames(size_t slot, uint64_t frames);

private:
    Clock::time_point start_time_{};
    std::array<uint64_t, kMaxTracks> baseline_frames_{};
};

} // namespace vr
