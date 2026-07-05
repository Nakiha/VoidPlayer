#pragma once

#include "renderer/buffer/track_buffer.h"

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
    uint64_t decode_stage_packet_send_count = 0;
    double decode_stage_packet_send_avg_ms = 0.0;
    double decode_stage_packet_send_max_ms = 0.0;
    uint64_t decode_stage_receive_loop_count = 0;
    uint64_t decode_stage_receive_frame_count = 0;
    double decode_stage_receive_avg_ms = 0.0;
    double decode_stage_receive_max_ms = 0.0;
    uint64_t decode_stage_convert_count = 0;
    double decode_stage_convert_avg_ms = 0.0;
    double decode_stage_convert_max_ms = 0.0;
    uint64_t decode_stage_convert_direct_planar_count = 0;
    double decode_stage_convert_direct_planar_avg_ms = 0.0;
    double decode_stage_convert_direct_planar_max_ms = 0.0;
    uint64_t decode_stage_convert_nv12_layout_count = 0;
    double decode_stage_convert_nv12_layout_avg_ms = 0.0;
    double decode_stage_convert_nv12_layout_max_ms = 0.0;
    uint64_t decode_stage_convert_nv12_alloc_count = 0;
    double decode_stage_convert_nv12_alloc_avg_ms = 0.0;
    double decode_stage_convert_nv12_alloc_max_ms = 0.0;
    uint64_t decode_stage_convert_nv12_pack_count = 0;
    double decode_stage_convert_nv12_pack_avg_ms = 0.0;
    double decode_stage_convert_nv12_pack_max_ms = 0.0;
    uint64_t decode_stage_publish_count = 0;
    double decode_stage_publish_avg_ms = 0.0;
    double decode_stage_publish_max_ms = 0.0;
    uint64_t decode_stage_publish_lock_count = 0;
    double decode_stage_publish_lock_avg_ms = 0.0;
    double decode_stage_publish_lock_max_ms = 0.0;
    uint64_t decode_stage_publish_wait_count = 0;
    double decode_stage_publish_wait_avg_ms = 0.0;
    double decode_stage_publish_wait_max_ms = 0.0;
    uint64_t decode_stage_publish_ring_push_count = 0;
    double decode_stage_publish_ring_push_avg_ms = 0.0;
    double decode_stage_publish_ring_push_max_ms = 0.0;
    uint64_t decode_stage_publish_ring_lock_count = 0;
    double decode_stage_publish_ring_lock_avg_ms = 0.0;
    double decode_stage_publish_ring_lock_max_ms = 0.0;
    uint64_t decode_stage_publish_ring_assign_count = 0;
    double decode_stage_publish_ring_assign_avg_ms = 0.0;
    double decode_stage_publish_ring_assign_max_ms = 0.0;
    uint64_t decode_stage_publish_ring_advance_count = 0;
    double decode_stage_publish_ring_advance_avg_ms = 0.0;
    double decode_stage_publish_ring_advance_max_ms = 0.0;
    uint64_t decode_stage_publish_ring_overwrite_count = 0;
    double decode_stage_publish_ring_overwrite_avg_bytes = 0.0;
    uint64_t decode_stage_publish_ring_overwrite_max_bytes = 0;
    uint64_t decode_stage_flush_count = 0;
    double decode_stage_flush_avg_ms = 0.0;
    double decode_stage_flush_max_ms = 0.0;
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
    FrameStorageKind current_frame_storage_kind = FrameStorageKind::Empty;
    int current_frame_yuv_bit_depth = 0;
    int current_frame_yuv_plane_layout = static_cast<int>(CpuYuvPlaneLayout::PlanarYuv420);
    int current_frame_yuv_sample_alignment =
        static_cast<int>(CpuYuvSampleAlignment::Packed);
};

} // namespace vr
