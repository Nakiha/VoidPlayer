#pragma once

#include "renderer/color/color_reference_types.h"

#include <cstdint>

namespace vr {

double color_reference_unorm_to_float(uint32_t value, int bit_depth);
ColorReferenceRgb color_reference_yuv_to_rgb_encoded(
    const ColorReferenceYuv& sample,
    const ColorReferenceConfig& config);
ColorReferenceRgb color_reference_map_to_output(
    const ColorReferenceRgb& rgb,
    const ColorReferenceConfig& config);
ColorReferenceRgb color_reference_map_to_windows_scrgb(
    const ColorReferenceRgb& rgb,
    int transfer,
    int primaries,
    double sdr_white_level_nits);
ColorReferenceRgb color_reference_sample_yuv(
    const ColorReferenceYuv& sample,
    const ColorReferenceConfig& config);

double color_reference_srgb_to_linear(double value);
double color_reference_linear_to_srgb(double value);
double color_reference_pq_to_linear_nits(double value);
double color_reference_hlg_to_linear(double value);

} // namespace vr
