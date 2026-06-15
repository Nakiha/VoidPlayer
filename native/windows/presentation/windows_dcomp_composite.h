#pragma once

#include <array>

namespace vr {

struct WindowsDcompCompositeSample {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

const char* windows_dcomp_composite_hlsl();

WindowsDcompCompositeSample composite_windows_dcomp_pixel(
    const WindowsDcompCompositeSample& video_linear_scrgb,
    const WindowsDcompCompositeSample& flutter_premultiplied_srgb,
    float sdr_white_scale);

float half_to_float(unsigned short value);

} // namespace vr
