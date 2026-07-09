#include "renderer/render/source_compositor_contract.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vr {
namespace {

bool valid_descriptor(const SourceCompositorTrackDescriptor& descriptor) {
    return descriptor.slot >= 0 &&
           descriptor.slot < kSourceCompositorMaxTracks &&
           descriptor.file_id >= 0 &&
           descriptor.width > 0 && descriptor.height > 0;
}

uint64_t estimate_surface_bytes(int width,
                                int height,
                                uint64_t bytes_per_pixel) {
    if (width <= 0 || height <= 0 || bytes_per_pixel == 0) {
        return 0;
    }
    const uint64_t pixels =
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixels > std::numeric_limits<uint64_t>::max() / bytes_per_pixel) {
        return 0;
    }
    return pixels * bytes_per_pixel;
}

int display_count_for_mode(int mode, int count) {
    return mode == 1 && count > 1 ? 2 : count;
}

} // namespace

SourceCompositorRingPolicy resolve_source_compositor_ring_policy(
    const std::vector<SourceCompositorTrackDescriptor>& descriptors,
    uint64_t budget_bytes,
    uint64_t bytes_per_pixel) {
    SourceCompositorRingPolicy result;
    if (descriptors.empty() ||
        descriptors.size() > kSourceCompositorMaxTracks ||
        budget_bytes == 0) {
        return result;
    }
    uint64_t bytes_per_frame = 0;
    std::array<bool, kSourceCompositorMaxTracks> slots{};
    for (const auto& descriptor : descriptors) {
        if (!valid_descriptor(descriptor) ||
            slots[static_cast<size_t>(descriptor.slot)]) {
            return result;
        }
        slots[static_cast<size_t>(descriptor.slot)] = true;
        const uint64_t bytes = estimate_surface_bytes(
            descriptor.width, descriptor.height, bytes_per_pixel);
        if (bytes == 0 ||
            bytes_per_frame > std::numeric_limits<uint64_t>::max() - bytes) {
            return result;
        }
        bytes_per_frame += bytes;
    }
    result.bytes_per_frame = bytes_per_frame;
    result.depth =
        bytes_per_frame <=
                budget_bytes / kSourceCompositorLiveBufferCount
            ? kSourceCompositorLiveBufferCount
            : 1;
    result.total_bytes = bytes_per_frame * static_cast<uint64_t>(result.depth);
    result.frozen_snapshot = result.depth == 1;
    result.allowed = result.total_bytes <= budget_bytes;
    return result;
}

SourceCompositorProjectionSample project_source_compositor_sample(
    float video_u,
    float video_v,
    const SourceCompositorProjection& projection,
    const std::array<bool, kSourceCompositorMaxTracks>& source_present) {
    SourceCompositorProjectionSample result;
    if (!projection.enabled) {
        return result;
    }
    const int count = std::clamp(
        projection.active_track_count, 1, kSourceCompositorMaxTracks);
    const int display_count = display_count_for_mode(projection.mode, count);
    int display_slot = 0;
    float local_u = video_u;
    if (projection.mode == 0 && display_count > 1) {
        const float scaled =
            std::clamp(video_u, 0.0f, 0.999999f) * display_count;
        display_slot = std::clamp(
            static_cast<int>(std::floor(scaled)), 0, display_count - 1);
        local_u = scaled - display_slot;
    } else if (projection.mode == 1 && display_count > 1) {
        display_slot =
            video_u < std::clamp(projection.split_pos, 0.0001f, 0.9999f)
                ? 0
                : 1;
    }
    const int source_slot = std::clamp(
        projection.source_order[static_cast<size_t>(display_slot)],
        0,
        kSourceCompositorMaxTracks - 1);
    if (!source_present[static_cast<size_t>(source_slot)]) {
        return result;
    }
    const float u =
        (local_u - projection.display_offset_x[source_slot]) *
            projection.inv_display_size_x[source_slot] -
        projection.view_offset_uv_x[source_slot];
    const float v =
        (video_v - projection.display_offset_y[source_slot]) *
            projection.inv_display_size_y[source_slot] -
        projection.view_offset_uv_y[source_slot];
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return result;
    }
    result.present = true;
    result.source_slot = source_slot;
    result.u = u;
    result.v = v;
    return result;
}

std::array<SourceCompositorRetainedVisualRect, kSourceCompositorMaxTracks>
project_source_compositor_retained_visuals(
    float viewport_left,
    float viewport_top,
    float viewport_right,
    float viewport_bottom,
    const SourceCompositorProjection& projection,
    const std::array<bool, kSourceCompositorMaxTracks>& source_present) {
    std::array<SourceCompositorRetainedVisualRect,
               kSourceCompositorMaxTracks> result{};
    if (!projection.enabled) {
        return result;
    }
    const float viewport_width =
        std::max(0.0f, viewport_right - viewport_left);
    const float viewport_height =
        std::max(0.0f, viewport_bottom - viewport_top);
    if (viewport_width <= 0.0f || viewport_height <= 0.0f) {
        return result;
    }
    const int count = std::clamp(
        projection.active_track_count, 1, kSourceCompositorMaxTracks);
    const int display_count = display_count_for_mode(projection.mode, count);
    for (int display_slot = 0; display_slot < display_count; ++display_slot) {
        const int source_slot = std::clamp(
            projection.source_order[static_cast<size_t>(display_slot)],
            0,
            kSourceCompositorMaxTracks - 1);
        if (!source_present[static_cast<size_t>(source_slot)]) {
            continue;
        }
        const float inv_x = projection.inv_display_size_x[source_slot];
        const float inv_y = projection.inv_display_size_y[source_slot];
        if (std::fabs(inv_x) < 0.00001f ||
            std::fabs(inv_y) < 0.00001f) {
            continue;
        }
        float projected_left = viewport_left;
        float projected_right = viewport_right;
        float clip_left = viewport_left;
        float clip_right = viewport_right;
        if (projection.mode == 0 && display_count > 1) {
            const float slot_width = viewport_width / display_count;
            projected_left = viewport_left + slot_width * display_slot;
            projected_right = projected_left + slot_width;
            clip_left = projected_left;
            clip_right = projected_right;
        } else if (projection.mode == 1 && display_count > 1) {
            const float split = std::clamp(
                projection.split_pos, 0.0001f, 0.9999f);
            const float split_x = viewport_left + viewport_width * split;
            if (display_slot == 0) {
                clip_left = viewport_left;
                clip_right = split_x;
            } else {
                clip_left = split_x;
                clip_right = viewport_right;
            }
        }
        const float projected_width =
            std::max(0.0f, projected_right - projected_left);
        if (projected_width <= 0.0f) {
            continue;
        }
        const float display_size_x = 1.0f / inv_x;
        const float display_size_y = 1.0f / inv_y;
        const float local_left =
            projection.display_offset_x[source_slot] +
            projection.view_offset_uv_x[source_slot] / inv_x;
        const float local_top =
            projection.display_offset_y[source_slot] +
            projection.view_offset_uv_y[source_slot] / inv_y;
        auto& rect = result[static_cast<size_t>(source_slot)];
        rect.present = true;
        rect.source_slot = source_slot;
        rect.left = projected_left + local_left * projected_width;
        rect.top = viewport_top + local_top * viewport_height;
        rect.right = rect.left + display_size_x * projected_width;
        rect.bottom = rect.top + display_size_y * viewport_height;
        rect.clip_left = clip_left;
        rect.clip_top = viewport_top;
        rect.clip_right = clip_right;
        rect.clip_bottom = viewport_bottom;
    }
    return result;
}

} // namespace vr
