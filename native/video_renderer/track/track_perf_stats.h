#pragma once

#include "video_renderer/buffer/track_buffer.h"

#include <cstddef>
#include <cstdint>

namespace vr {

/// Per-track performance stats snapshot returned to the UI layer.
struct TrackPerfStats {
    int slot = -1;
    int file_id = 0;
    uint64_t frames_decoded = 0;
    double fps = 0.0;
    double avg_decode_ms = 0.0;
    double max_decode_ms = 0.0;
    size_t buffer_count = 0;
    size_t buffer_capacity = 0;
    TrackState buffer_state = TrackState::Empty;
    int64_t current_pts_us = 0;
    int64_t current_dts_us = kNoTimestampUs;
    int32_t analysis_frame_index = kInvalidAnalysisFrameIndex;
    int32_t source_packet_index = kInvalidSourcePacketIndex;
    int32_t source_packet_size = 0;
    int64_t source_packet_pos = kUnknownSourcePacketPosition;
    int64_t source_packet_pts = kNoTimestampUs;
    int64_t source_packet_dts = kNoTimestampUs;
    FrameIdentityMode frame_identity_mode = FrameIdentityMode::Unknown;
};

} // namespace vr
