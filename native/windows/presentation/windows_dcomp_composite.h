#pragma once

#include <array>

namespace vr {

struct WindowsDcompCompositeSample {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

struct WindowsSourceProjection {
    bool enabled = false;
    int mode = 0;
    float split_pos = 0.5f;
    int active_track_count = 1;
    std::array<int, 4> source_order = {0, 1, 2, 3};
    std::array<float, 4> display_offset_x{};
    std::array<float, 4> display_offset_y{};
    std::array<float, 4> inv_display_size_x{};
    std::array<float, 4> inv_display_size_y{};
    std::array<float, 4> view_offset_uv_x{};
    std::array<float, 4> view_offset_uv_y{};
};

struct WindowsSourceProjectionSample {
    bool present = false;
    int source_slot = -1;
    float u = 0.0f;
    float v = 0.0f;
};

struct WindowsRetainedSourceVisualRect {
    bool present = false;
    int source_slot = -1;
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float clip_left = 0.0f;
    float clip_top = 0.0f;
    float clip_right = 0.0f;
    float clip_bottom = 0.0f;
};

const char* windows_dcomp_composite_hlsl();

WindowsDcompCompositeSample composite_windows_dcomp_pixel(
    const WindowsDcompCompositeSample& video_linear_scrgb,
    const WindowsDcompCompositeSample& flutter_premultiplied_srgb,
    float sdr_white_scale);

float half_to_float(unsigned short value);

WindowsSourceProjectionSample project_windows_source_sample(
    float video_u,
    float video_v,
    const WindowsSourceProjection& projection,
    const std::array<bool, 4>& source_present);

std::array<WindowsRetainedSourceVisualRect, 4>
project_windows_retained_source_visuals(
    float viewport_left,
    float viewport_top,
    float viewport_right,
    float viewport_bottom,
    const WindowsSourceProjection& projection,
    const std::array<bool, 4>& source_present);

} // namespace vr
