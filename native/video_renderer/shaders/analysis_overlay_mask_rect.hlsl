// Analysis overlay GPU mask materialization pass.

#include "common.hlsl"

struct AnalysisOverlayRect {
    uint rect_uv0;
    uint rect_uv1;
    uint color_bgra;
    uint track_idx;
};

StructuredBuffer<AnalysisOverlayRect> u_analysis_overlay_rects : register(t28);

struct VSMaskOutput {
    float4 position : SV_POSITION;
    float2 video_uv : TEXCOORD0;
    nointerpolation float4 rect_uv : TEXCOORD1;
};

float2 rect_corner(uint vertex_id) {
    if (vertex_id == 0) return float2(0.0, 1.0);
    if (vertex_id == 1) return float2(0.0, 0.0);
    if (vertex_id == 2) return float2(1.0, 1.0);
    return float2(1.0, 0.0);
}

float2 unpack_uv16(uint packed) {
    return float2(
        float(packed & 0xffffu) / 65535.0,
        float((packed >> 16) & 0xffffu) / 65535.0);
}

VSMaskOutput VSMain(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    AnalysisOverlayRect rect = u_analysis_overlay_rects[instance_id];
    float2 rect_min = unpack_uv16(rect.rect_uv0);
    float2 rect_max = unpack_uv16(rect.rect_uv1);
    float2 video_uv = lerp(rect_min, rect_max, rect_corner(vertex_id));

    VSMaskOutput output;
    output.position = float4(video_uv.x * 2.0 - 1.0, 1.0 - video_uv.y * 2.0, 0.0, 1.0);
    output.video_uv = video_uv;
    output.rect_uv = float4(rect_min, rect_max);
    return output;
}

float4 PSMain(VSMaskOutput input) : SV_TARGET {
    float2 px = max(fwidth(input.video_uv), float2(1.0 / 65535.0, 1.0 / 65535.0));
    float left = input.video_uv.x - input.rect_uv.x;
    float top = input.video_uv.y - input.rect_uv.y;
    float right = input.rect_uv.z - input.video_uv.x;
    float bottom = input.rect_uv.w - input.video_uv.y;
    float2 border_distance = float2(min(left, right), min(top, bottom));
    if (border_distance.x <= px.x * 0.75 || border_distance.y <= px.y * 0.75) {
        return float4(1.0, 0.0, 0.0, 1.0);
    }
    discard;
    return float4(0.0, 0.0, 0.0, 0.0);
}
