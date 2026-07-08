#include "renderer/render/presentation_source_projection.h"

#include "renderer/layout/layout_state.h"

#include <algorithm>
#include <cmath>

namespace vr {
namespace {

int display_count_for_mode(int mode, int active_track_count) {
    const int count = std::clamp(active_track_count, 1, 4);
    if (mode == 1) {
        return std::min(count, 2);
    }
    return count;
}

} // namespace

bool validate_presentation_source_projection(
    const PresentationSourceProjection& projection,
    const std::array<bool, 4>& source_present,
    std::string* error) {
    const auto fail = [&](const char* message) {
        if (error) {
            *error = message;
        }
        return false;
    };
    if (!projection.enabled) {
        return true;
    }
    if (projection.mode != LAYOUT_SIDE_BY_SIDE &&
        projection.mode != LAYOUT_SPLIT_SCREEN) {
        return fail("source projection mode is invalid");
    }
    if (!std::isfinite(projection.split_pos) ||
        projection.split_pos < 0.0f ||
        projection.split_pos > 1.0f) {
        return fail("source projection split position is invalid");
    }
    if (projection.active_track_count < 1 ||
        projection.active_track_count > 4) {
        return fail("source projection active track count is invalid");
    }
    std::array<bool, 4> used{};
    for (int i = 0; i < projection.active_track_count; ++i) {
        const int slot = projection.source_order[static_cast<size_t>(i)];
        if (slot < 0 || slot >= 4) {
            return fail("source projection order references an invalid slot");
        }
        if (used[static_cast<size_t>(slot)]) {
            return fail("source projection order contains duplicate slots");
        }
        if (!source_present[static_cast<size_t>(slot)]) {
            return fail("source projection order references an unavailable slot");
        }
        used[static_cast<size_t>(slot)] = true;
    }
    for (size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(projection.display_offset_x[i]) ||
            !std::isfinite(projection.display_offset_y[i]) ||
            !std::isfinite(projection.inv_display_size_x[i]) ||
            !std::isfinite(projection.inv_display_size_y[i]) ||
            !std::isfinite(projection.view_offset_uv_x[i]) ||
            !std::isfinite(projection.view_offset_uv_y[i])) {
            return fail("source projection transform contains non-finite values");
        }
    }
    return true;
}

PresentationSourceProjectionSample project_presentation_source_sample(
    float video_u,
    float video_v,
    const PresentationSourceProjection& projection,
    const std::array<bool, 4>& source_present) {
    PresentationSourceProjectionSample result;
    if (!projection.enabled) {
        return result;
    }
    const int count = std::clamp(projection.active_track_count, 1, 4);
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
        projection.source_order[static_cast<size_t>(display_slot)], 0, 3);
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

std::array<PresentationRetainedSourceVisualRect, 4>
project_presentation_retained_source_visuals(
    float viewport_left,
    float viewport_top,
    float viewport_right,
    float viewport_bottom,
    const PresentationSourceProjection& projection,
    const std::array<bool, 4>& source_present) {
    std::array<PresentationRetainedSourceVisualRect, 4> result{};
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
    const int count = std::clamp(projection.active_track_count, 1, 4);
    const int display_count = display_count_for_mode(projection.mode, count);
    for (int display_slot = 0; display_slot < display_count; ++display_slot) {
        const int source_slot = std::clamp(
            projection.source_order[static_cast<size_t>(display_slot)], 0, 3);
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
        const float projected_height = viewport_height;
        const float source_width = projected_width / inv_x;
        const float source_height = projected_height / inv_y;
        const float left = projected_left +
            projected_width * projection.display_offset_x[source_slot] +
            source_width * projection.view_offset_uv_x[source_slot];
        const float top = viewport_top +
            projected_height * projection.display_offset_y[source_slot] +
            source_height * projection.view_offset_uv_y[source_slot];
        auto& rect = result[static_cast<size_t>(source_slot)];
        rect.present = true;
        rect.source_slot = source_slot;
        rect.left = left;
        rect.top = top;
        rect.right = left + source_width;
        rect.bottom = top + source_height;
        rect.clip_left = clip_left;
        rect.clip_top = viewport_top;
        rect.clip_right = clip_right;
        rect.clip_bottom = viewport_bottom;
    }
    return result;
}

} // namespace vr
