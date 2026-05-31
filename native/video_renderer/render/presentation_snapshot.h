#pragma once

#include "video_renderer/frame/frame_storage.h"
#include "video_renderer/layout/layout_geometry.h"
#include "video_renderer/render/shader_constants.h"
#include "video_renderer/sync/render_sink.h"

#include <array>
#include <cstdint>

namespace vr {

struct PresentationFrameSnapshot {
    bool present = false;
    int file_id = -1;
    uint64_t track_generation = 0;
    FrameStorageKind storage_kind = FrameStorageKind::Empty;
    int width = 0;
    int height = 0;
    int64_t pts_us = 0;
    int64_t dts_us = kNoTimestampUs;
    int64_t duration_us = 0;
    int32_t analysis_frame_index = kInvalidAnalysisFrameIndex;
    int32_t frame_identity_mode = static_cast<int32_t>(FrameIdentityMode::Unknown);
    int32_t source_packet_index = kInvalidSourcePacketIndex;
    int32_t source_packet_size = 0;
    int64_t source_packet_pos = kUnknownSourcePacketPosition;
    int64_t source_packet_pts = kNoTimestampUs;
    int64_t source_packet_dts = kNoTimestampUs;
    bool is_nv12 = false;
    bool is_p010 = false;
    int y_stride = 0;
    int uv_stride = 0;
    int coded_width = 0;
    int coded_height = 0;
    float nv12_uv_scale_x = 1.0f;
    float nv12_uv_scale_y = 1.0f;
    int color_range = VIDEO_COLOR_RANGE_LIMITED;
    int color_matrix = VIDEO_COLOR_MATRIX_BT709;
    int color_transfer = VIDEO_COLOR_TRANSFER_SDR;
    int color_primaries = VIDEO_COLOR_PRIMARIES_BT709;
};

using PresentationFrameSnapshotList =
    std::array<PresentationFrameSnapshot, kMaxTracks>;

struct PresentationSnapshot {
    bool should_present = false;
    int frame_count = 0;
    int64_t current_pts_us = 0;
    ShaderConstants constants{};
    PresentationFrameSnapshotList frames{};
};

int default_presentation_color_matrix_for_size(int width, int height);
int default_presentation_color_primaries_for_matrix(int color_matrix);

PresentationSnapshot build_presentation_snapshot(
    const PresentDecision& decision,
    const LayoutState& layout,
    const LayoutTrackGeometryList& track_geometry,
    int canvas_width,
    int canvas_height,
    const float background_color[4] = nullptr);

} // namespace vr
