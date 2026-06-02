// Multitrack video renderer shader entry points.
// Compiled with an embedded D3DInclude handler.

#include "common.hlsl"
#include "color_pipeline.hlsl"
#include "sampling.hlsl"

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.texcoord = input.texcoord;
    return output;
}

float4 PSMain(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    int track_idx;
    float2 local_uv;

    if (u_mode == MODE_SPLIT_SCREEN) {
        if (texcoord.x < u_split_pos) {
            track_idx = u_order[0];
        } else {
            track_idx = u_order[1];
        }
        local_uv = texcoord;
    } else {
        int count = max(u_track_count, 1);
        float scaled_x = texcoord.x * float(count);
        int slot = int(scaled_x);
        slot = clamp(slot, 0, count - 1);
        track_idx = u_order[slot];
        local_uv = float2(scaled_x - float(slot), texcoord.y);
    }

    track_idx = clamp(track_idx, 0, 3);

    bool out_of_bounds;
    float2 tex_uv = calc_aspect_fit_uv(local_uv, track_idx, out_of_bounds);

    float4 color = out_of_bounds
        ? u_background_color
        : sample_track(track_idx, tex_uv);

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
