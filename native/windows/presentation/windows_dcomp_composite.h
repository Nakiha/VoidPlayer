#pragma once

#include "renderer/render/source_compositor_contract.h"

#include <array>

namespace vr {

struct WindowsDcompCompositeSample {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

using WindowsSourceProjection = SourceCompositorProjection;
using WindowsSourceProjectionSample = SourceCompositorProjectionSample;
using WindowsRetainedSourceVisualRect = SourceCompositorRetainedVisualRect;

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
