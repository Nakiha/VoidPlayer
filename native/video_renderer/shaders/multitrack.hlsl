// Multitrack video renderer shaders
// Supports RGBA and NV12 (D3D11VA) texture inputs
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

// RGBA textures (software decode path)
Texture2D u_textures[4] : register(t0);
SamplerState u_sampler : register(s0);

// NV12 Y plane textures (D3D11VA hardware decode path)
Texture2D<float> u_textures_y[4] : register(t4);
// NV12 UV plane textures (D3D11VA hardware decode path)
Texture2D<float2> u_textures_uv[4] : register(t8);

cbuffer Constants : register(b0) {
    int u_track_count;          // offset 0
    float u_canvas_width;       // offset 4
    float u_canvas_height;      // offset 8
    float _pad0;                // offset 12 (padding to 16-byte boundary)
    float4 u_video_aspect;      // offset 16 (16-byte boundary)
    int u_nv12_mask;            // offset 32: bit i set = track i uses NV12
    float3 _pad1;               // offset 36-47: padding (float3 avoids per-element 16-byte alignment)
    float4 u_nv12_uv_scale_y;   // offset 48-63: video_h / texture_h (float4 vector, tightly packed)
};

// BT.601 YUV → RGB conversion (standard definition)
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

float4 PSMain(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    int track_idx = 0;
    float2 adjusted_uv = texcoord;

    if (u_track_count == 1) {
        track_idx = 0;
    }
    else if (u_track_count == 2) {
        // Side by side
        if (texcoord.x < 0.5) {
            track_idx = 0;
            adjusted_uv.x = texcoord.x * 2.0;
        } else {
            track_idx = 1;
            adjusted_uv.x = (texcoord.x - 0.5) * 2.0;
        }
    }
    else if (u_track_count >= 3) {
        // 2x2 grid
        int row = texcoord.y < 0.5 ? 0 : 1;
        int col = texcoord.x < 0.5 ? 0 : 1;
        track_idx = row * 2 + col;
        if (track_idx >= u_track_count) {
            track_idx = u_track_count - 1;
        }
        adjusted_uv.x = (texcoord.x < 0.5) ? texcoord.x * 2.0 : (texcoord.x - 0.5) * 2.0;
        adjusted_uv.y = (texcoord.y < 0.5) ? texcoord.y * 2.0 : (texcoord.y - 0.5) * 2.0;
    }

    // Aspect ratio correction (letterbox)
    float video_aspect = u_video_aspect[track_idx];
    float region_aspect = 1.0;
    if (u_track_count == 2) {
        region_aspect = (u_canvas_width * 0.5) / u_canvas_height;
    } else if (u_track_count >= 3) {
        region_aspect = (u_canvas_width * 0.5) / (u_canvas_height * 0.5);
    } else {
        region_aspect = u_canvas_width / u_canvas_height;
    }

    float aspect_ratio = video_aspect / region_aspect;
    if (aspect_ratio > 1.0) {
        // Video wider than region - letterbox vertically
        adjusted_uv.y = (adjusted_uv.y - 0.5) / aspect_ratio + 0.5;
    } else if (aspect_ratio < 1.0) {
        // Video taller than region - letterbox horizontally
        adjusted_uv.x = (adjusted_uv.x - 0.5) * aspect_ratio + 0.5;
    }

    // Clamp to avoid sampling outside
    if (adjusted_uv.x < 0.0 || adjusted_uv.x > 1.0 ||
        adjusted_uv.y < 0.0 || adjusted_uv.y > 1.0) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    return sample_track(track_idx, adjusted_uv);
}
