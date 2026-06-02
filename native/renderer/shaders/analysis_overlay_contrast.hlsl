// Analysis overlay contrast assist pass.

#include "common.hlsl"

Texture2D u_analysis_overlay_mask[4] : register(t24);

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.texcoord = input.texcoord;
    return output;
}

float sample_overlay_mask(float2 uv) {
    return max(
        max(u_analysis_overlay_mask[0].Sample(u_sampler, uv).r,
            u_analysis_overlay_mask[1].Sample(u_sampler, uv).r),
        max(u_analysis_overlay_mask[2].Sample(u_sampler, uv).r,
            u_analysis_overlay_mask[3].Sample(u_sampler, uv).r));
}

float4 PSMain(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    float center = sample_overlay_mask(texcoord);
    float2 texel = float2(
        u_canvas_width > 0.0 ? 1.0 / u_canvas_width : 0.0,
        u_canvas_height > 0.0 ? 1.0 / u_canvas_height : 0.0);
    float halo = max(
        max(sample_overlay_mask(texcoord + float2(texel.x, 0.0)),
            sample_overlay_mask(texcoord - float2(texel.x, 0.0))),
        max(sample_overlay_mask(texcoord + float2(0.0, texel.y)),
            sample_overlay_mask(texcoord - float2(0.0, texel.y))));

    if (center >= 0.5) {
        return float4(1.0, 1.0, 1.0, 0.45);
    }

    if (halo < 0.5) {
        discard;
    }

    return float4(0.0, 0.0, 0.0, 0.65);
}
