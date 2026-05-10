#pragma once

#include <cstddef>

namespace vr {

constexpr size_t kShaderConstantsSize = 304;

struct ShaderConstants {
    int mode;              // offset 0
    int track_count;       // offset 4
    float split_pos;       // offset 8
    float zoom_ratio;      // offset 12
    float canvas_width;    // offset 16
    float canvas_height;   // offset 20
    float view_offset[2];  // offset 24
    int order[4];          // offset 32
    float video_aspect[4]; // offset 48
    int nv12_mask;         // offset 64
    int planar_yuv_mask;   // offset 68
    float _pad1[2];        // offset 72
    float nv12_uv_scale_y[4]; // offset 80
    float nv12_uv_scale_x[4]; // offset 96
    float track_scale[4];  // offset 112
    float display_offset_x[4];     // offset 128
    float display_offset_y[4];     // offset 144
    float inv_display_size_x[4];   // offset 160
    float inv_display_size_y[4];   // offset 176
    float view_offset_uv_x[4];     // offset 192
    float view_offset_uv_y[4];     // offset 208
    float background_color[4];     // offset 224
    int color_range[4];            // offset 240
    int color_matrix[4];           // offset 256
    int color_transfer[4];         // offset 272
    int color_primaries[4];        // offset 288
};

static_assert(sizeof(ShaderConstants) == kShaderConstantsSize,
              "ShaderConstants must match multitrack.hlsl cbuffer size");
static_assert(offsetof(ShaderConstants, mode) == 0);
static_assert(offsetof(ShaderConstants, canvas_width) == 16);
static_assert(offsetof(ShaderConstants, order) == 32);
static_assert(offsetof(ShaderConstants, video_aspect) == 48);
static_assert(offsetof(ShaderConstants, nv12_mask) == 64);
static_assert(offsetof(ShaderConstants, planar_yuv_mask) == 68);
static_assert(offsetof(ShaderConstants, nv12_uv_scale_y) == 80);
static_assert(offsetof(ShaderConstants, track_scale) == 112);
static_assert(offsetof(ShaderConstants, display_offset_x) == 128);
static_assert(offsetof(ShaderConstants, background_color) == 224);
static_assert(offsetof(ShaderConstants, color_range) == 240);
static_assert(offsetof(ShaderConstants, color_primaries) == 288);

} // namespace vr
