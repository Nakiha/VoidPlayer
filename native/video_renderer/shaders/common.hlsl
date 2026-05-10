// Shared declarations for the multitrack renderer shader.

struct VSInput {
    float2 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

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

// RGBA textures (legacy/direct texture path)
Texture2D u_textures[4] : register(t0);
SamplerState u_sampler : register(s0);

// NV12/P010 Y plane textures (software upload and D3D11VA decode paths)
Texture2D<float> u_textures_y[4] : register(t4);
// NV12/P010 UV plane textures (software upload and D3D11VA decode paths)
Texture2D<float2> u_textures_uv[4] : register(t8);

// Planar YUV software upload textures.
Texture2D<float> u_planar_u[4] : register(t12);
Texture2D<float> u_planar_v[4] : register(t16);

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
    // Use int4 (16 bytes) not int[4] (64 bytes): HLSL arrays each take a full 16-byte register.
    int4 u_order;              // offset 32: track display order mapping

    // === Track aspects (offset 48-63) ===
    float4 u_video_aspect;     // offset 48: aspect ratio for each track

    // === YUV layout params (offset 64-111) ===
    int u_nv12_mask;           // offset 64: bit i set = track i uses NV12/P010
    int u_planar_yuv_mask;     // offset 68: bit i set = track i uses planar Y/U/V
    float2 _pad1;              // offset 72-79
    float4 u_nv12_uv_scale_y;  // offset 80-95: video_h / texture_h
    float4 u_nv12_uv_scale_x;  // offset 96-111: video_w / texture_w

    // === Uniform pixel density (offset 112-127) ===
    float4 u_track_scale;      // offset 112: per-track scale for uniform pixel density

    // === Precomputed display params (offset 128-223) ===
    // Computed on CPU from video_aspect, slot_aspect, zoom_ratio, track_scale, view_offset.
    // The pixel shader uses these directly, avoiding per-pixel recomputation.
    float4 u_display_offset_x;    // offset 128: display_offset.x for track 0-3
    float4 u_display_offset_y;    // offset 144
    float4 u_inv_display_size_x;  // offset 160
    float4 u_inv_display_size_y;  // offset 176
    float4 u_view_offset_uv_x;    // offset 192
    float4 u_view_offset_uv_y;    // offset 208
    float4 u_background_color;    // offset 224
    int4 u_color_range;           // offset 240: VideoColorRange per track
    int4 u_color_matrix;          // offset 256: VideoColorMatrix per track
    int4 u_color_transfer;        // offset 272: VideoColorTransfer per track
    int4 u_color_primaries;       // offset 288: VideoColorPrimaries per track
};
// Total: 304 bytes. Keep in sync with shader_constants.h.
