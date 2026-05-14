#pragma once

#include "video_renderer/renderer.h"

#include <cmath>

namespace vr {

struct LayoutValidationResult {
    bool ok = true;
    const char* message = "";
};

inline LayoutValidationResult validate_layout_state(const LayoutState& state) {
    if (state.mode != LAYOUT_SIDE_BY_SIDE && state.mode != LAYOUT_SPLIT_SCREEN) {
        return {false, "layout mode out of range"};
    }
    if (state.pixel_size_mode != PIXEL_SIZE_UNIFORM_VIDEO_PIXELS &&
        state.pixel_size_mode != PIXEL_SIZE_FILL_VIEW) {
        return {false, "pixel size mode out of range"};
    }
    if (!std::isfinite(state.split_pos) ||
        !std::isfinite(state.zoom_ratio) ||
        !std::isfinite(state.view_offset[0]) ||
        !std::isfinite(state.view_offset[1])) {
        return {false, "layout values must be finite"};
    }
    if (state.split_pos < kMinLayoutSplitPos || state.split_pos > kMaxLayoutSplitPos) {
        return {false, "split position out of range"};
    }
    if (state.zoom_ratio < kMinLayoutZoomRatio ||
        state.zoom_ratio > kMaxLayoutZoomRatio) {
        return {false, "zoom ratio out of range"};
    }
    for (int i = 0; i < 4; ++i) {
        const int file_id = state.order[i];
        if (file_id < -1) {
            return {false, "layout order file_id out of range"};
        }
        if (file_id <= 0) {
            continue;
        }
        for (int j = 0; j < i; ++j) {
            if (state.order[j] == file_id) {
                return {false, "layout order contains duplicate file_id"};
            }
        }
    }
    return {};
}

} // namespace vr
