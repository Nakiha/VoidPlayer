#pragma once

namespace vr {

/// Layout mode constants (match HLSL defines).
constexpr int LAYOUT_SIDE_BY_SIDE = 0;
constexpr int LAYOUT_SPLIT_SCREEN = 1;
constexpr float kMinLayoutSplitPos = 0.0f;
constexpr float kMaxLayoutSplitPos = 1.0f;
constexpr float kMinLayoutZoomRatio = 1.0f;
constexpr float kMaxLayoutZoomRatio = 50.0f;

/// Viewport pixel-size policy constants (match Flutter protocol).
constexpr int PIXEL_SIZE_UNIFORM_VIDEO_PIXELS = 0;
constexpr int PIXEL_SIZE_FILL_VIEW = 1;

/// Layout state - all visual layout parameters in one struct.
/// Updated atomically via Renderer::apply_layout().
struct LayoutState {
    int mode = LAYOUT_SIDE_BY_SIDE;  // 0=SIDE_BY_SIDE, 1=SPLIT_SCREEN
    float split_pos = 0.5f;          // Split divider position (0.0-1.0)
    float zoom_ratio = 1.0f;         // 1.0=fit, >1.0=zoom in
    float view_offset[2] = {0.0f, 0.0f};  // Pan offset in pixel coordinates
    int pixel_size_mode = PIXEL_SIZE_UNIFORM_VIDEO_PIXELS;  // 0=uniform density, 1=fit each slot
    int order[4] = {0, 1, 2, 3};    // Track display order by file_id; -1/0 are placeholders
};

} // namespace vr
