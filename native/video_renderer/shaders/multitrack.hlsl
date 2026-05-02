// Multitrack video renderer shaders
// Supports RGBA and NV12 (D3D11VA) texture inputs
// Supports SIDE_BY_SIDE (equal split) and SPLIT_SCREEN (overlapping with divider) modes
// Supports viewport zoom and pan
// Compiled at runtime via D3DCompile

// ---- Vertex Shader ----

struct VSInput {
    float2 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.texcoord = input.texcoord;
    return output;
}

// ---- Pixel Shader ----

// Layout modes
#define MODE_SIDE_BY_SIDE 0
#define MODE_SPLIT_SCREEN 1

#define COLOR_RANGE_UNKNOWN 0
#define COLOR_RANGE_LIMITED 1
#define COLOR_RANGE_FULL 2

#define COLOR_MATRIX_UNKNOWN 0
#define COLOR_MATRIX_BT601 1
#define COLOR_MATRIX_BT709 2
#define COLOR_MATRIX_BT2020_NCL 3

#define COLOR_TRANSFER_UNKNOWN 0
#define COLOR_TRANSFER_SDR 1
#define COLOR_TRANSFER_PQ 2
#define COLOR_TRANSFER_HLG 3

#define COLOR_PRIMARIES_UNKNOWN 0
#define COLOR_PRIMARIES_BT601 1
#define COLOR_PRIMARIES_BT709 2
#define COLOR_PRIMARIES_BT2020 3

// RGBA textures (software decode path)
Texture2D u_textures[4] : register(t0);
SamplerState u_sampler : register(s0);

// NV12 Y plane textures (D3D11VA hardware decode path)
Texture2D<float> u_textures_y[4] : register(t4);
// NV12 UV plane textures (D3D11VA hardware decode path)
Texture2D<float2> u_textures_uv[4] : register(t8);

cbuffer Constants : register(b0) {
    // === Layout params (offset 0-15) ===
    int u_mode;                // offset 0:  0=SIDE_BY_SIDE, 1=SPLIT_SCREEN
    int u_track_count;         // offset 4
    float u_split_pos;         // offset 8:  split divider position (0.0-1.0)
    float u_zoom_ratio;        // offset 12: zoom ratio (1.0=fit, >1.0=zoom in)

    // === Canvas params (offset 16-31) ===
    float u_canvas_width;      // offset 16
    float u_canvas_height;     // offset 20
    float2 u_view_offset;      // offset 24: pan offset in pixel coordinates

    // === Track order (offset 32-47) ===
    // Use int4 (16 bytes) not int[4] (64 bytes) — HLSL arrays each take a full 16-byte register
    int4 u_order;              // offset 32: track display order mapping

    // === Track aspects (offset 48-63) ===
    float4 u_video_aspect;     // offset 48: aspect ratio for each track

    // === NV12 params (offset 64-95) ===
    int u_nv12_mask;           // offset 64: bit i set = track i uses NV12
    float3 _pad1;              // offset 68-79
    float4 u_nv12_uv_scale_y;  // offset 80-95: video_h / texture_h

    // === Uniform pixel density (offset 96-111) ===
    float4 u_track_scale;      // offset 96: per-track scale for uniform pixel density

    // === Precomputed display params (offset 112-207) ===
    // Computed on CPU from video_aspect, slot_aspect, zoom_ratio, track_scale, view_offset.
    // The pixel shader uses these directly, avoiding per-pixel recomputation.
    float4 u_display_offset_x;    // offset 112: display_offset.x for track 0-3
    float4 u_display_offset_y;    // offset 128: display_offset.y for track 0-3
    float4 u_inv_display_size_x;  // offset 144: 1/display_size.x for track 0-3
    float4 u_inv_display_size_y;  // offset 160: 1/display_size.y for track 0-3
    float4 u_view_offset_uv_x;   // offset 176: view_offset_uv.x for track 0-3
    float4 u_view_offset_uv_y;   // offset 192: view_offset_uv.y for track 0-3
    float4 u_background_color;   // offset 208: viewport fill outside video bounds
    int4 u_color_range;          // offset 224: VideoColorRange per track
    int4 u_color_matrix;         // offset 240: VideoColorMatrix per track
    int4 u_color_transfer;       // offset 256: VideoColorTransfer per track
    int4 u_color_primaries;      // offset 272: VideoColorPrimaries per track
};
// Total: 288 bytes — must match renderer.cpp draw_frame() Constants struct (288 bytes)

float3 linear_to_srgb(float3 x) {
    x = max(x, 0.0);
    float3 lo = x * 12.92;
    float3 hi = 1.055 * pow(x, 1.0 / 2.4) - 0.055;
    return lerp(lo, hi, step(0.0031308, x));
}

float3 srgb_to_linear(float3 x) {
    x = saturate(x);
    float3 lo = x / 12.92;
    float3 hi = pow((x + 0.055) / 1.055, 2.4);
    return lerp(lo, hi, step(0.04045, x));
}

float3 convert_linear_primaries_to_bt709(float3 rgb, int primaries) {
    if (primaries == COLOR_PRIMARIES_BT2020) {
        return float3(
            1.6605 * rgb.r - 0.5876 * rgb.g - 0.0728 * rgb.b,
           -0.1246 * rgb.r + 1.1329 * rgb.g - 0.0083 * rgb.b,
           -0.0182 * rgb.r - 0.1006 * rgb.g + 1.1187 * rgb.b);
    }
    return rgb;
}

float3 pq_to_linear_nits(float3 x) {
    x = saturate(x);
    const float m1 = 0.1593017578125;  // 2610 / 16384
    const float m2 = 78.84375;         // 2523 / 32
    const float c1 = 0.8359375;        // 3424 / 4096
    const float c2 = 18.8515625;       // 2413 / 128
    const float c3 = 18.6875;          // 2392 / 128
    float3 p = pow(x, 1.0 / m2);
    float3 num = max(p - c1, 0.0);
    float3 den = max(c2 - c3 * p, 1e-6);
    return pow(num / den, 1.0 / m1) * 10000.0;
}

float3 hlg_to_linear(float3 x) {
    x = saturate(x);
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    float3 lo = (x * x) / 3.0;
    float3 hi = (exp((x - c) / a) + b) / 12.0;
    return lerp(lo, hi, step(0.5, x));
}

float3 tone_map_to_sdr(float3 rgb, int transfer, int primaries) {
    if (transfer == COLOR_TRANSFER_PQ) {
        float3 lin = pq_to_linear_nits(rgb) / 203.0;
        lin = convert_linear_primaries_to_bt709(lin, primaries);
        return saturate(linear_to_srgb(lin / (1.0 + lin)));
    }
    if (transfer == COLOR_TRANSFER_HLG) {
        float3 lin = hlg_to_linear(rgb) * 4.0;
        lin = convert_linear_primaries_to_bt709(lin, primaries);
        return saturate(linear_to_srgb(lin / (1.0 + lin)));
    }
    if (primaries == COLOR_PRIMARIES_BT2020) {
        float3 lin = convert_linear_primaries_to_bt709(srgb_to_linear(rgb), primaries);
        return saturate(linear_to_srgb(lin));
    }
    return saturate(rgb);
}

float3 yuv_to_rgb(float y, float2 uv, int range, int color_matrix, int transfer, int primaries) {
    float y_full = y;
    float2 cbcr = uv - 0.5;

    if (range != COLOR_RANGE_FULL) {
        y_full = (y - (16.0 / 255.0)) * (255.0 / 219.0);
        cbcr *= (255.0 / 224.0);
    }

    float cb = cbcr.x;
    float cr = cbcr.y;
    float3 rgb;
    if (color_matrix == COLOR_MATRIX_BT2020_NCL) {
        rgb = float3(
            y_full + 1.4746 * cr,
            y_full - 0.164553 * cb - 0.571353 * cr,
            y_full + 1.8814 * cb);
    } else if (color_matrix == COLOR_MATRIX_BT709 || color_matrix == COLOR_MATRIX_UNKNOWN) {
        rgb = float3(
            y_full + 1.5748 * cr,
            y_full - 0.187324 * cb - 0.468124 * cr,
            y_full + 1.8556 * cb);
    } else {
        rgb = float3(
            y_full + 1.402 * cr,
            y_full - 0.344136 * cb - 0.714136 * cr,
            y_full + 1.772 * cb);
    }

    return tone_map_to_sdr(rgb, transfer, primaries);
}

// Sample a track's texture and return RGBA color.
// Uses NV12 path if the corresponding bit in u_nv12_mask is set.
float4 sample_track(int track_idx, float2 uv) {
    // NV12 hardware decode path
    if (u_nv12_mask & (1 << track_idx)) {
        float y;
        float2 uv_color;

        // Scale UV.y to crop D3D11VA alignment padding at the bottom of the texture
        float2 scaled_uv = float2(uv.x, uv.y * u_nv12_uv_scale_y[track_idx]);

        // SM 5.0 requires literal index for texture array .Sample()
        if (track_idx == 0) {
            y = u_textures_y[0].Sample(u_sampler, scaled_uv);
            uv_color = u_textures_uv[0].Sample(u_sampler, scaled_uv);
        } else if (track_idx == 1) {
            y = u_textures_y[1].Sample(u_sampler, scaled_uv);
            uv_color = u_textures_uv[1].Sample(u_sampler, scaled_uv);
        } else if (track_idx == 2) {
            y = u_textures_y[2].Sample(u_sampler, scaled_uv);
            uv_color = u_textures_uv[2].Sample(u_sampler, scaled_uv);
        } else {
            y = u_textures_y[3].Sample(u_sampler, scaled_uv);
            uv_color = u_textures_uv[3].Sample(u_sampler, scaled_uv);
        }

        return float4(
            yuv_to_rgb(
                y,
                uv_color,
                u_color_range[track_idx],
                u_color_matrix[track_idx],
                u_color_transfer[track_idx],
                u_color_primaries[track_idx]),
            1.0);
    }

    // RGBA software decode path
    float4 color = float4(0.0, 0.0, 0.0, 1.0);
    if (track_idx == 0)      color = u_textures[0].Sample(u_sampler, uv);
    else if (track_idx == 1) color = u_textures[1].Sample(u_sampler, uv);
    else if (track_idx == 2) color = u_textures[2].Sample(u_sampler, uv);
    else                     color = u_textures[3].Sample(u_sampler, uv);
    return color;
}

// Calculate texture UV from slot-local UV using precomputed per-track constants.
// All heavy math (fit_scale, display_size, zoom, pan) is done on the CPU side.
// Returns: video texture UV, or sets out_of_bounds=true if outside video area
float2 calc_aspect_fit_uv(
    float2 local_uv,
    int track_idx,
    out bool out_of_bounds
) {
    out_of_bounds = false;

    float2 display_offset = float2(u_display_offset_x[track_idx], u_display_offset_y[track_idx]);
    float2 inv_display_size = float2(u_inv_display_size_x[track_idx], u_inv_display_size_y[track_idx]);
    float2 view_offset_uv = float2(u_view_offset_uv_x[track_idx], u_view_offset_uv_y[track_idx]);

    float2 offset_uv = (local_uv - display_offset) * inv_display_size - view_offset_uv;

    if (offset_uv.x < 0.0 || offset_uv.x > 1.0 ||
        offset_uv.y < 0.0 || offset_uv.y > 1.0) {
        out_of_bounds = true;
        return float2(0.0, 0.0);
    }

    return offset_uv;
}

float4 PSMain(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    int track_idx;
    float2 local_uv;

    if (u_mode == MODE_SPLIT_SCREEN) {
        // SPLIT_SCREEN: two overlapping videos, split by divider
        if (texcoord.x < u_split_pos) {
            track_idx = u_order[0];
        } else {
            track_idx = u_order[1];
        }
        local_uv = texcoord;
    } else {
        // SIDE_BY_SIDE: equal width 1/N split
        int count = max(u_track_count, 1);
        float scaled_x = texcoord.x * float(count);
        int slot = int(scaled_x);
        slot = clamp(slot, 0, count - 1);
        track_idx = u_order[slot];
        local_uv = float2(scaled_x - float(slot), texcoord.y);
    }

    // Clamp track index to valid range [0, 3]
    track_idx = clamp(track_idx, 0, 3);

    // Calculate aspect-fit UV using precomputed per-track constants
    bool out_of_bounds;
    float2 tex_uv = calc_aspect_fit_uv(local_uv, track_idx, out_of_bounds);

    float4 color = out_of_bounds
        ? u_background_color
        : sample_track(track_idx, tex_uv);

    // SPLIT_SCREEN: render a hard inverted divider. Alpha-blended inversion
    // turns mid-tone footage into a gray seam, so keep the core fully inverted.
    if (u_mode == MODE_SPLIT_SCREEN && u_canvas_width > 0.0) {
        float divider_x = u_split_pos * u_canvas_width;
        float pixel_x = texcoord.x * u_canvas_width;
        float dist = abs(pixel_x - divider_x);
        float core_width = 1.25;
        float edge_width = 0.75;

        if (dist <= core_width + edge_width) {
            float alpha = (dist <= core_width)
                ? 1.0
                : 1.0 - ((dist - core_width) / edge_width);
            float3 divider_color = 1.0 - color.rgb;
            color.rgb = divider_color * alpha + color.rgb * (1.0 - alpha);
            color.a = 1.0;
        }
    }

    return color;
}
