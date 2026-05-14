#pragma once

#include "media/seek_controller.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace vr {

struct PendingSeekPreviewEventState {
    bool has_request = false;
    bool emitted = true;
    int64_t target_pts_us = -1;
};

struct SeekTargetResolution {
    int64_t requested_pts_us = 0;
    int64_t target_pts_us = 0;
    int64_t effective_duration_us = 0;
    bool clamped = false;
    bool retarget_pending_event = false;
};

SeekTargetResolution resolve_seek_target(
    int64_t requested_pts_us,
    int64_t effective_duration_us,
    const PendingSeekPreviewEventState& pending_event);

struct HevcSeekRecreateInput {
    bool is_hevc_hw_seek = false;
    bool paused_seek = false;
    bool seek_transition_active = false;
    bool recreated_for_paused_hevc_seek = false;
    bool force_recreate_paused_hevc = false;
    SeekType seek_type = SeekType::Keyframe;
};

struct HevcSeekRecreateDecision {
    bool should_recreate_pipeline = false;
    bool error_if_recreate_not_applied = false;
    bool coalescing_transition = false;
};

HevcSeekRecreateDecision choose_hevc_seek_recreate(
    const HevcSeekRecreateInput& input);

struct LoopRangeSeekInput {
    bool playing = false;
    bool loop_enabled = false;
    bool clock_paused = true;
    int64_t current_pts_us = 0;
    int64_t start_us = 0;
    int64_t end_us = 0;
};

struct LoopRangeSeekDecision {
    bool should_seek = false;
    int64_t target_pts_us = 0;
};

LoopRangeSeekDecision choose_loop_range_seek(
    const LoopRangeSeekInput& input);

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
