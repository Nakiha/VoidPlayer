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
        !std::isfinite(state.view_offset[1]) ||
        state.zoom_ratio <= 0.0f) {
        return {false, "layout values must be finite and zoom must be positive"};
    }
    return {};
}

} // namespace vr
