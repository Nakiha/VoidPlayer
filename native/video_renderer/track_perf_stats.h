#pragma once

#include "video_renderer/buffer/track_buffer.h"

#include <cstddef>
#include <cstdint>

namespace vr {

/// Per-track performance stats snapshot returned to the UI layer.
struct TrackPerfStats {
    int slot = -1;
    int file_id = 0;
    double fps = 0.0;
    double avg_decode_ms = 0.0;
    double max_decode_ms = 0.0;
    size_t buffer_count = 0;
    size_t buffer_capacity = 0;
    TrackState buffer_state = TrackState::Empty;
    int64_t current_pts_us = 0;
    int64_t current_dts_us = kNoTimestampUs;
};

} // namespace vr
