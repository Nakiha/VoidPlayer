#pragma once

#include "media/seek_controller.h"
#include "video_renderer/seek_coordinator.h"
#include "video_renderer/track_lifecycle.h"

#include <cstddef>
#include <cstdint>

namespace vr {

struct SeekRequestLogFacts {
    double target_seconds = 0.0;
    const char* type_label = "Keyframe";
};

SeekRequestLogFacts build_seek_request_log_facts(
    int64_t target_pts_us,
    SeekType type);

struct SeekClampLogFacts {
    bool should_log = false;
    double requested_seconds = 0.0;
    double clamped_seconds = 0.0;
    double duration_seconds = 0.0;
};

SeekClampLogFacts build_seek_clamp_log_facts(
    const SeekTargetResolution& resolution);

struct TrackSeekTargetClampLogFacts {
    bool should_log = false;
    size_t slot = 0;
    double requested_seconds = 0.0;
    double clamped_seconds = 0.0;
};

TrackSeekTargetClampLogFacts build_track_seek_target_clamp_log_facts(
    size_t slot,
    const TrackSeekTargetResolution& target);

struct TrackSeekCoalescingLogFacts {
    bool should_log = false;
    size_t slot = 0;
    int buffer_state_before = 0;
    double target_seconds = 0.0;
};

TrackSeekCoalescingLogFacts build_track_seek_coalescing_log_facts(
    size_t slot,
    const TrackSeekTargetResolution& target,
    const TrackSeekPreparationResult& preparation,
    const TrackSeekExecutionResult& execution);

struct TrackSeekClearedLogFacts {
    bool should_log = false;
    size_t slot = 0;
    size_t buffered_frames_before = 0;
    size_t buffered_frames_after = 0;
    size_t packet_queue_size_before = 0;
    double target_seconds = 0.0;
};

TrackSeekClearedLogFacts build_track_seek_cleared_log_facts(
    size_t slot,
    const TrackSeekTargetResolution& target,
    const TrackSeekPreparationResult& preparation,
    const TrackSeekExecutionResult& execution,
    size_t buffered_frames_after);

} // namespace vr
