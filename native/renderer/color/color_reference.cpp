#include "renderer/color/color_reference.h"

#include "renderer/frame/frame_storage.h"

#include <algorithm>
#include <cmath>

namespace vr {
namespace {

double saturate(double value) {
    return std::clamp(value, 0.0, 1.0);
}

double non_negative(double value) {
    return std::max(value, 0.0);
}

ColorReferenceRgb saturate_rgb(ColorReferenceRgb rgb) {
    rgb.r = saturate(rgb.r);
    rgb.g = saturate(rgb.g);
    rgb.b = saturate(rgb.b);
    return rgb;
}

ColorReferenceRgb max_rgb(ColorReferenceRgb rgb, double floor) {
    rgb.r = std::max(rgb.r, floor);
    rgb.g = std::max(rgb.g, floor);
    rgb.b = std::max(rgb.b, floor);
    return rgb;
}

ColorReferenceRgb convert_linear_primaries_to_bt709(
    ColorReferenceRgb rgb,
    int primaries) {
    if (primaries == VIDEO_COLOR_PRIMARIES_BT2020) {
        return {
            1.6605 * rgb.r - 0.5876 * rgb.g - 0.0728 * rgb.b,
            -0.1246 * rgb.r + 1.1329 * rgb.g - 0.0083 * rgb.b,
            -0.0182 * rgb.r - 0.1006 * rgb.g + 1.1187 * rgb.b,
        };
    }
    return rgb;
}

ColorReferenceRgb srgb_to_linear(ColorReferenceRgb rgb) {
    return {
        color_reference_srgb_to_linear(rgb.r),
        color_reference_srgb_to_linear(rgb.g),
        color_reference_srgb_to_linear(rgb.b),
    };
}

ColorReferenceRgb linear_to_srgb(ColorReferenceRgb rgb) {
    return {
        color_reference_linear_to_srgb(rgb.r),
        color_reference_linear_to_srgb(rgb.g),
        color_reference_linear_to_srgb(rgb.b),
    };
}

ColorReferenceRgb pq_to_linear_nits(ColorReferenceRgb rgb) {
    return {
        color_reference_pq_to_linear_nits(rgb.r),
        color_reference_pq_to_linear_nits(rgb.g),
        color_reference_pq_to_linear_nits(rgb.b),
    };
}

ColorReferenceRgb hlg_to_linear(ColorReferenceRgb rgb) {
    return {
        color_reference_hlg_to_linear(rgb.r),
        color_reference_hlg_to_linear(rgb.g),
        color_reference_hlg_to_linear(rgb.b),
    };
}

ColorReferenceRgb scale(ColorReferenceRgb rgb, double factor) {
    rgb.r *= factor;
    rgb.g *= factor;
    rgb.b *= factor;
    return rgb;
}

ColorReferenceRgb reinhard(ColorReferenceRgb rgb) {
    return {
        rgb.r / (1.0 + rgb.r),
        rgb.g / (1.0 + rgb.g),
        rgb.b / (1.0 + rgb.b),
    };
}

ColorReferenceRgb tone_map_to_sdr(ColorReferenceRgb rgb,
                                  int transfer,
                                  int primaries) {
    if (transfer == VIDEO_COLOR_TRANSFER_PQ) {
        auto lin = scale(pq_to_linear_nits(rgb), 1.0 / 203.0);
        lin = convert_linear_primaries_to_bt709(lin, primaries);
        return saturate_rgb(linear_to_srgb(reinhard(lin)));
    }
    if (transfer == VIDEO_COLOR_TRANSFER_HLG) {
        auto lin = scale(hlg_to_linear(rgb), 4.0);
        lin = convert_linear_primaries_to_bt709(lin, primaries);
        return saturate_rgb(linear_to_srgb(reinhard(lin)));
    }
    if (primaries == VIDEO_COLOR_PRIMARIES_BT2020) {
        auto lin = convert_linear_primaries_to_bt709(srgb_to_linear(rgb), primaries);
        return saturate_rgb(linear_to_srgb(lin));
    }
    return saturate_rgb(rgb);
}

ColorReferenceRgb map_to_edr(ColorReferenceRgb rgb,
                             int transfer,
                             int primaries) {
    if (transfer == VIDEO_COLOR_TRANSFER_PQ) {
        auto lin = scale(pq_to_linear_nits(rgb), 1.0 / 203.0);
        return max_rgb(convert_linear_primaries_to_bt709(lin, primaries), 0.0);
    }
    if (transfer == VIDEO_COLOR_TRANSFER_HLG) {
        auto lin = scale(hlg_to_linear(rgb), 4.0);
        return max_rgb(convert_linear_primaries_to_bt709(lin, primaries), 0.0);
    }
    if (primaries == VIDEO_COLOR_PRIMARIES_BT2020) {
        auto lin = convert_linear_primaries_to_bt709(srgb_to_linear(rgb), primaries);
        return max_rgb(lin, 0.0);
    }
    return srgb_to_linear(rgb);
}

} // namespace

double color_reference_unorm_to_float(uint32_t value, int bit_depth) {
    if (bit_depth <= 0 || bit_depth > 16) {
        return 0.0;
    }
    const uint32_t max_value = (1u << bit_depth) - 1u;
    return static_cast<double>(std::min(value, max_value)) /
        static_cast<double>(max_value);
}

ColorReferenceRgb color_reference_yuv_to_rgb_encoded(
    const ColorReferenceYuv& sample,
    const ColorReferenceConfig& config) {
    double y_full = sample.y;
    double cb = sample.u - 0.5;
    double cr = sample.v - 0.5;
    if (config.range != VIDEO_COLOR_RANGE_FULL) {
        y_full = (sample.y * 255.0 - 16.0) / 219.0;
        cb = (sample.u * 255.0 - 128.0) / 224.0;
        cr = (sample.v * 255.0 - 128.0) / 224.0;
    }

    ColorReferenceRgb rgb;
    if (config.matrix == VIDEO_COLOR_MATRIX_BT2020_NCL) {
        rgb = {
            y_full + 1.4746 * cr,
            y_full - 0.164553 * cb - 0.571353 * cr,
            y_full + 1.8814 * cb,
        };
    } else if (config.matrix == VIDEO_COLOR_MATRIX_BT709 ||
               config.matrix == VIDEO_COLOR_MATRIX_UNKNOWN) {
        rgb = {
            y_full + 1.5748 * cr,
            y_full - 0.187324 * cb - 0.468124 * cr,
            y_full + 1.8556 * cb,
        };
    } else {
        rgb = {
            y_full + 1.402 * cr,
            y_full - 0.344136 * cb - 0.714136 * cr,
            y_full + 1.772 * cb,
        };
    }
    if (config.transfer == VIDEO_COLOR_TRANSFER_SDR) {
        rgb.r -= 1.0 / 255.0;
        rgb.g -= 1.0 / 255.0;
        rgb.b -= 1.0 / 255.0;
    }
    return rgb;
}

ColorReferenceRgb color_reference_map_to_output(
    const ColorReferenceRgb& rgb,
    const ColorReferenceConfig& config) {
    if (config.output_edr) {
        return map_to_edr(rgb, config.transfer, config.primaries);
    }
    return tone_map_to_sdr(rgb, config.transfer, config.primaries);
}

ColorReferenceRgb color_reference_sample_yuv(
    const ColorReferenceYuv& sample,
    const ColorReferenceConfig& config) {
    return color_reference_map_to_output(
        color_reference_yuv_to_rgb_encoded(sample, config),
        config);
}

double color_reference_srgb_to_linear(double value) {
    value = saturate(value);
    if (value < 0.04045) {
        return value / 12.92;
    }
    return std::pow((value + 0.055) / 1.055, 2.4);
}

double color_reference_linear_to_srgb(double value) {
    value = non_negative(value);
    if (value < 0.0031308) {
        return value * 12.92;
    }
    return 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
}

double color_reference_pq_to_linear_nits(double value) {
    value = saturate(value);
    constexpr double m1 = 0.1593017578125;
    constexpr double m2 = 78.84375;
    constexpr double c1 = 0.8359375;
    constexpr double c2 = 18.8515625;
    constexpr double c3 = 18.6875;
    const double p = std::pow(value, 1.0 / m2);
    const double num = std::max(p - c1, 0.0);
    const double den = std::max(c2 - c3 * p, 1e-6);
    return std::pow(num / den, 1.0 / m1) * 10000.0;
}

double color_reference_hlg_to_linear(double value) {
    value = saturate(value);
    constexpr double a = 0.17883277;
    constexpr double b = 0.28466892;
    constexpr double c = 0.55991073;
    if (value < 0.5) {
        return (value * value) / 3.0;
    }
    return (std::exp((value - c) / a) + b) / 12.0;
}

} // namespace vr
