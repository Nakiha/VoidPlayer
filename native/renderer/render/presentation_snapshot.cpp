#include "renderer/render/presentation_snapshot.h"

namespace vr {
namespace {

int normalized_color_range(const TextureFrame& frame) {
    return frame.color.range != VIDEO_COLOR_RANGE_UNKNOWN
        ? frame.color.range
        : VIDEO_COLOR_RANGE_LIMITED;
}

int normalized_color_matrix(const TextureFrame& frame) {
    return frame.color.matrix != VIDEO_COLOR_MATRIX_UNKNOWN
        ? frame.color.matrix
        : default_presentation_color_matrix_for_size(frame.width, frame.height);
}

int normalized_color_transfer(const TextureFrame& frame) {
    return frame.color.transfer != VIDEO_COLOR_TRANSFER_UNKNOWN
        ? frame.color.transfer
        : VIDEO_COLOR_TRANSFER_SDR;
}

int normalized_color_primaries(const TextureFrame& frame, int color_matrix) {
    return frame.color.primaries != VIDEO_COLOR_PRIMARIES_UNKNOWN
        ? frame.color.primaries
        : default_presentation_color_primaries_for_matrix(color_matrix);
}

void fill_frame_storage_snapshot(const TextureFrame& frame,
                                 PresentationFrameSnapshot& out) {
    out.storage_kind = frame.storage_kind();
    out.storage_class = frame.storage_class();
    out.is_nv12 = frame.is_nv12;
    out.is_p010 = frame.is_p010;

    if (const auto* nv12_storage = frame.cpu_nv12_storage()) {
        out.is_nv12 = true;
        out.is_p010 = nv12_storage->is_p010;
        out.yuv_format = nv12_storage->is_p010
            ? PRESENTATION_YUV_FORMAT_P010
            : PRESENTATION_YUV_FORMAT_NV12;
        out.y_stride = nv12_storage->y_stride;
        out.uv_stride = nv12_storage->uv_stride;
        out.coded_width = nv12_storage->coded_width;
        out.coded_height = nv12_storage->coded_height;
        if (nv12_storage->coded_width > 0) {
            out.nv12_uv_scale_x =
                static_cast<float>(frame.width) / static_cast<float>(nv12_storage->coded_width);
        }
        if (nv12_storage->coded_height > 0) {
            out.nv12_uv_scale_y =
                static_cast<float>(frame.height) / static_cast<float>(nv12_storage->coded_height);
        }
    } else if (const auto* planar_storage = frame.cpu_planar_yuv_storage()) {
        const bool semiplanar =
            planar_storage->plane_layout == CpuYuvPlaneLayout::SemiPlanarYuv420;
        out.is_nv12 = semiplanar;
        out.is_p010 = semiplanar &&
            planar_storage->sample_alignment == CpuYuvSampleAlignment::MsbAligned;
        if (semiplanar) {
            out.yuv_format = out.is_p010
                ? PRESENTATION_YUV_FORMAT_P010
                : PRESENTATION_YUV_FORMAT_NV12;
        } else {
            out.yuv_format = planar_storage->bit_depth >= 10
                ? PRESENTATION_YUV_FORMAT_YUV420P10LE
                : PRESENTATION_YUV_FORMAT_YUV420P;
        }
        out.y_stride = planar_storage->strides[0];
        out.uv_stride = planar_storage->strides[1];
        out.coded_width = planar_storage->plane_widths[0];
        out.coded_height = planar_storage->plane_heights[0];
        if (out.coded_width > 0) {
            out.nv12_uv_scale_x =
                static_cast<float>(frame.width) / static_cast<float>(out.coded_width);
        }
        if (out.coded_height > 0) {
            out.nv12_uv_scale_y =
                static_cast<float>(frame.height) / static_cast<float>(out.coded_height);
        }
    } else if (const auto* cv_storage = frame.cv_pixel_buffer_storage()) {
        out.is_nv12 = true;
        out.is_p010 = cv_storage->is_p010;
        out.yuv_format = cv_storage->is_p010
            ? PRESENTATION_YUV_FORMAT_P010
            : PRESENTATION_YUV_FORMAT_NV12;
        out.coded_width = cv_storage->coded_width;
        out.coded_height = cv_storage->coded_height;
        if (cv_storage->coded_width > 0) {
            out.nv12_uv_scale_x =
                static_cast<float>(frame.width) / static_cast<float>(cv_storage->coded_width);
        }
        if (cv_storage->coded_height > 0) {
            out.nv12_uv_scale_y =
                static_cast<float>(frame.height) / static_cast<float>(cv_storage->coded_height);
        }
    }
}

} // namespace

int default_presentation_color_matrix_for_size(int width, int height) {
    return width >= 1280 || height > 576
        ? VIDEO_COLOR_MATRIX_BT709
        : VIDEO_COLOR_MATRIX_BT601;
}

int default_presentation_color_primaries_for_matrix(int color_matrix) {
    if (color_matrix == VIDEO_COLOR_MATRIX_BT2020_NCL) {
        return VIDEO_COLOR_PRIMARIES_BT2020;
    }
    if (color_matrix == VIDEO_COLOR_MATRIX_BT601) {
        return VIDEO_COLOR_PRIMARIES_BT601;
    }
    return VIDEO_COLOR_PRIMARIES_BT709;
}

PresentationSnapshot build_presentation_snapshot(
    const PresentDecision& decision,
    const LayoutState& layout,
    const LayoutTrackGeometryList& track_geometry,
    int canvas_width,
    int canvas_height,
    const float background_color[4]) {
    PresentationSnapshot snapshot;
    snapshot.should_present = decision.should_present;
    snapshot.current_pts_us = decision.current_pts_us;

    populate_layout_shader_constants(
        snapshot.constants,
        layout,
        track_geometry,
        canvas_width,
        canvas_height);

    snapshot.constants.nv12_mask = 0;
    snapshot.constants.planar_yuv_mask = 0;
    if (background_color) {
        for (int i = 0; i < 4; ++i) {
            snapshot.constants.background_color[i] = background_color[i];
        }
    }

    for (size_t slot = 0; slot < kMaxTracks; ++slot) {
        auto& frame_out = snapshot.frames[slot];
        frame_out.file_id = decision.file_ids[slot];
        frame_out.track_generation = decision.track_generations[slot];

        if (!decision.frames[slot].has_value()) {
            snapshot.constants.nv12_uv_scale_x[slot] = 1.0f;
            snapshot.constants.nv12_uv_scale_y[slot] = 1.0f;
            snapshot.constants.color_range[slot] = frame_out.color_range;
            snapshot.constants.color_matrix[slot] = frame_out.color_matrix;
            snapshot.constants.color_transfer[slot] = frame_out.color_transfer;
            snapshot.constants.color_primaries[slot] = frame_out.color_primaries;
            continue;
        }

        const auto& frame = *decision.frames[slot];
        frame_out.present = true;
        frame_out.width = frame.width;
        frame_out.height = frame.height;
        frame_out.pts_us = frame.pts_us;
        frame_out.dts_us = frame.dts_us;
        frame_out.duration_us = frame.duration_us;
        frame_out.analysis_frame_index = frame.analysis_frame_index;
        frame_out.frame_identity_mode = static_cast<int32_t>(frame.frame_identity_mode);
        frame_out.source_packet_index = frame.source_packet_index;
        frame_out.source_packet_size = frame.source_packet_size;
        frame_out.source_packet_pos = frame.source_packet_pos;
        frame_out.source_packet_pts = frame.source_packet_pts;
        frame_out.source_packet_dts = frame.source_packet_dts;
        fill_frame_storage_snapshot(frame, frame_out);
        frame_out.color_range = normalized_color_range(frame);
        frame_out.color_matrix = normalized_color_matrix(frame);
        frame_out.color_transfer = normalized_color_transfer(frame);
        frame_out.color_primaries =
            normalized_color_primaries(frame, frame_out.color_matrix);

        snapshot.constants.color_range[slot] = frame_out.color_range;
        snapshot.constants.color_matrix[slot] = frame_out.color_matrix;
        snapshot.constants.color_transfer[slot] = frame_out.color_transfer;
        snapshot.constants.color_primaries[slot] = frame_out.color_primaries;
        snapshot.constants.nv12_uv_scale_x[slot] = frame_out.nv12_uv_scale_x;
        snapshot.constants.nv12_uv_scale_y[slot] = frame_out.nv12_uv_scale_y;
        if (frame.cpu_planar_yuv_storage()) {
            snapshot.constants.planar_yuv_mask |= (1 << static_cast<int>(slot));
        } else if (frame_out.is_nv12) {
            snapshot.constants.nv12_mask |= (1 << static_cast<int>(slot));
        }
        ++snapshot.frame_count;
    }

    return snapshot;
}

} // namespace vr
