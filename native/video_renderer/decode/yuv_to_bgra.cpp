#include "video_renderer/decode/yuv_to_bgra.h"

#include <algorithm>

namespace vr {
namespace {

uint8_t clamp_u8(int value) {
    return static_cast<uint8_t>(std::max(0, std::min(255, value)));
}

struct YuvMatrixCoefficients {
    int r_v = 409;
    int g_u = -100;
    int g_v = -208;
    int b_u = 516;
};

YuvMatrixCoefficients coefficients_for_matrix(int color_matrix, bool full_range) {
    switch (color_matrix) {
    case VIDEO_COLOR_MATRIX_BT709:
        return full_range
            ? YuvMatrixCoefficients{403, -48, -120, 475}
            : YuvMatrixCoefficients{459, -55, -136, 541};
    case VIDEO_COLOR_MATRIX_BT2020_NCL:
        return full_range
            ? YuvMatrixCoefficients{378, -42, -146, 482}
            : YuvMatrixCoefficients{430, -48, -167, 548};
    case VIDEO_COLOR_MATRIX_BT601:
    case VIDEO_COLOR_MATRIX_UNKNOWN:
    default:
        return full_range
            ? YuvMatrixCoefficients{359, -88, -183, 454}
            : YuvMatrixCoefficients{409, -100, -208, 516};
    }
}

}  // namespace

int default_yuv_color_matrix_for_size(int width, int height) {
    return width >= 1280 || height > 576
        ? VIDEO_COLOR_MATRIX_BT709
        : VIDEO_COLOR_MATRIX_BT601;
}

void write_yuv_to_bgra(uint8_t y,
                       uint8_t u,
                       uint8_t v,
                       int color_range,
                       int color_matrix,
                       uint8_t* out) {
    if (!out) {
        return;
    }
    const bool full_range = color_range == VIDEO_COLOR_RANGE_FULL;
    if (color_matrix == VIDEO_COLOR_MATRIX_UNKNOWN) {
        color_matrix = VIDEO_COLOR_MATRIX_BT601;
    }
    const YuvMatrixCoefficients c = coefficients_for_matrix(color_matrix, full_range);
    const int uu = static_cast<int>(u) - 128;
    const int vv = static_cast<int>(v) - 128;
    const int yy = full_range
        ? static_cast<int>(y) * 256
        : 298 * std::max(0, static_cast<int>(y) - 16);

    const int r = (yy + c.r_v * vv + 128) >> 8;
    const int g = (yy + c.g_u * uu + c.g_v * vv + 128) >> 8;
    const int b = (yy + c.b_u * uu + 128) >> 8;
    out[0] = clamp_u8(b);
    out[1] = clamp_u8(g);
    out[2] = clamp_u8(r);
    out[3] = 255;
}

}  // namespace vr
