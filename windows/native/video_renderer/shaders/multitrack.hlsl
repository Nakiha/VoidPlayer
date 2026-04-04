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
};
// Total: 96 bytes

// BT.601 YUV -> RGB conversion (standard definition)
float3 yuv_to_rgb(float y, float2 uv) {
    float cb = uv.x - 0.5;
    float cr = uv.y - 0.5;
    float r = saturate(y + 1.402 * cr);
    float g = saturate(y - 0.344136 * cb - 0.714136 * cr);
    float b = saturate(y + 1.772 * cb);
    return float3(r, g, b);
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

        return float4(yuv_to_rgb(y, uv_color), 1.0);
    }

    // RGBA software decode path
    float4 color = float4(0.0, 0.0, 0.0, 1.0);
    if (track_idx == 0)      color = u_textures[0].Sample(u_sampler, uv);
    else if (track_idx == 1) color = u_textures[1].Sample(u_sampler, uv);
    else if (track_idx == 2) color = u_textures[2].Sample(u_sampler, uv);
    else                     color = u_textures[3].Sample(u_sampler, uv);
    return color;
}

// Calculate aspect-fit UV with zoom and pan.
// slot_aspect: aspect ratio of the allocated display region
// video_aspect: aspect ratio of the video
// local_uv: UV within the slot (0-1)
// Returns: video texture UV, or sets out_of_bounds=true if outside video area
float2 calc_aspect_fit_uv(
    float slot_aspect,
    float video_aspect,
    float2 local_uv,
    out bool out_of_bounds
) {
    out_of_bounds = false;

    if (video_aspect <= 0.0 || slot_aspect <= 0.0) {
        return local_uv;
    }

    // 1. Compute fit scale factor
    float fit_scale;
    if (video_aspect > slot_aspect) {
        fit_scale = 1.0 / video_aspect * slot_aspect;
    } else {
        fit_scale = 1.0;
    }

    // 2. Apply zoom (zoom_ratio=1.0 is fit, >1.0 is zoom in)
    float display_scale = fit_scale * u_zoom_ratio;

    // 3. Compute display region within slot (centered)
    float2 display_size = float2(
        video_aspect * display_scale / slot_aspect,
        display_scale
    );
    float2 display_offset = (float2(1.0, 1.0) - display_size) * 0.5;

    // 4. Map local UV to display UV
    float2 display_uv = local_uv - display_offset;

    // 5. Normalize to video UV (0-1)
    float2 normalized_uv = display_uv / max(display_size, float2(0.0001, 0.0001));

    // 6. Apply view offset (pixel coords -> UV space)
    // Both axes negated: mouse drag right shows left content, drag down shows upper content
    float2 canvas_size = float2(u_canvas_width, u_canvas_height);
    float2 offset_uv = normalized_uv - u_view_offset / canvas_size;

    // 7. Check bounds
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
    float slot_aspect;

    if (u_mode == MODE_SPLIT_SCREEN) {
        // SPLIT_SCREEN: two overlapping videos, split by divider
        // Only uses first 2 tracks from u_order
        if (texcoord.x < u_split_pos) {
            track_idx = u_order[0];
        } else {
            track_idx = u_order[1];
        }
        // UV covers full canvas (no region splitting)
        local_uv = texcoord;
        float canvas_aspect = u_canvas_width / u_canvas_height;
        slot_aspect = canvas_aspect;
    } else {
        // SIDE_BY_SIDE: equal width 1/N split
        int count = max(u_track_count, 1);
        int slot = int(texcoord.x * float(count));
        slot = clamp(slot, 0, count - 1);
        track_idx = u_order[slot];
        local_uv = float2(texcoord.x * float(count) - float(slot), texcoord.y);
        slot_aspect = (u_canvas_width / float(count)) / u_canvas_height;
    }

    // Clamp track index
    track_idx = clamp(track_idx, 0, max(u_track_count - 1, 0));

    // Get video aspect ratio
    float video_aspect = u_video_aspect[track_idx];
    if (video_aspect <= 0.0) {
        video_aspect = slot_aspect;
    }

    // Calculate aspect-fit UV with zoom and pan
    bool out_of_bounds;
    float2 tex_uv = calc_aspect_fit_uv(slot_aspect, video_aspect, local_uv, out_of_bounds);

    if (out_of_bounds) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float4 color = sample_track(track_idx, tex_uv);

    // SPLIT_SCREEN: render divider line (2px width)
    if (u_mode == MODE_SPLIT_SCREEN && u_canvas_width > 0.0) {
        float divider_x = u_split_pos * u_canvas_width;
        float pixel_x = texcoord.x * u_canvas_width;
        float half_width = 1.0;

        if (abs(pixel_x - divider_x) <= half_width) {
            float alpha = 0.8 * (1.0 - abs(pixel_x - divider_x) / half_width);
            color = float4(1.0, 1.0, 1.0, alpha) * alpha + color * (1.0 - alpha);
        }
    }

    return color;
}
