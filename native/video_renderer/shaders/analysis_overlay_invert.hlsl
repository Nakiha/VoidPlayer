// Analysis overlay invert-mask pass.

#include "common.hlsl"
#include "color_pipeline.hlsl"
#include "sampling.hlsl"

Texture2D u_analysis_overlay_mask[4] : register(t24);

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.texcoord = input.texcoord;
    return output;
}

float sample_analysis_overlay_mask(int track_idx, float2 uv) {
    if (track_idx == 0) return u_analysis_overlay_mask[0].Sample(u_sampler, uv).r;
    if (track_idx == 1) return u_analysis_overlay_mask[1].Sample(u_sampler, uv).r;
    if (track_idx == 2) return u_analysis_overlay_mask[2].Sample(u_sampler, uv).r;
    return u_analysis_overlay_mask[3].Sample(u_sampler, uv).r;
}

bool resolve_analysis_overlay_uv(float2 texcoord, out int track_idx, out float2 uv) {
    float2 local_uv;
    if (u_mode == MODE_SPLIT_SCREEN) {
        track_idx = texcoord.x < u_split_pos ? u_order.x : u_order.y;
        local_uv = texcoord;
    } else {
        int count = max(u_track_count, 1);
        float scaled_x = texcoord.x * float(count);
        int slot = clamp(int(scaled_x), 0, count - 1);
        track_idx = u_order[slot];
        local_uv = float2(scaled_x - float(slot), texcoord.y);
    }

    track_idx = clamp(track_idx, 0, 3);
    bool out_of_bounds;
    uv = calc_aspect_fit_uv(local_uv, track_idx, out_of_bounds);
    return !out_of_bounds;
}

float4 PSMain(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    int track_idx;
    float2 uv;
    if (!resolve_analysis_overlay_uv(texcoord, track_idx, uv)) {
        discard;
    }

    if (sample_analysis_overlay_mask(track_idx, uv) < 0.5) {
        discard;
    }
    return float4(1.0, 1.0, 1.0, 1.0);
}
