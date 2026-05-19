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

float4 PSMain(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    float mask = max(
        max(u_analysis_overlay_mask[0].Sample(u_sampler, texcoord).r,
            u_analysis_overlay_mask[1].Sample(u_sampler, texcoord).r),
        max(u_analysis_overlay_mask[2].Sample(u_sampler, texcoord).r,
            u_analysis_overlay_mask[3].Sample(u_sampler, texcoord).r));
    if (mask < 0.5) {
        discard;
    }
    return float4(1.0, 1.0, 1.0, 1.0);
}
