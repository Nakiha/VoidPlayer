#pragma once

#include "media/seek_controller.h"

#include <chrono>
#include <optional>

namespace vr {

class SeekCoordinator {
public:
    explicit SeekCoordinator(std::chrono::milliseconds paused_hevc_settle_delay);

    void reset();
    bool should_defer_paused_hevc_seek(bool playing,
                                       bool has_hevc_hw_track,
                                       int64_t target_pts_us,
                                       SeekType type);
    std::optional<SeekRequest> take_deferred_paused_hevc_seek(bool playing);
    void mark_paused_hevc_preview_drawn(bool has_hevc_hw_track);

    bool paused_hevc_seek_in_flight() const { return paused_hevc_seek_in_flight_; }
    bool has_pending_deferred_seek() const { return deferred_paused_hevc_seek_.has_value(); }

private:
    using Clock = std::chrono::steady_clock;

    const std::chrono::milliseconds paused_hevc_settle_delay_;
    std::optional<SeekRequest> deferred_paused_hevc_seek_;
    bool paused_hevc_seek_in_flight_ = false;
    bool paused_hevc_initial_settle_done_ = false;
    Clock::time_point paused_hevc_seek_settle_until_{};
};

} // namespace vr
