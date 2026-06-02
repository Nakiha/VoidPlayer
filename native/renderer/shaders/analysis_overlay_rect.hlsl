// Analysis overlay instanced rect pass.

#include "common.hlsl"
#include "color_pipeline.hlsl"
#include "sampling.hlsl"

struct AnalysisOverlayRect {
    uint rect_uv0;
    uint rect_uv1;
    uint color_bgra;
    uint track_idx;
};

StructuredBuffer<AnalysisOverlayRect> u_analysis_overlay_rects : register(t28);

struct VSRectOutput {
    float4 position : SV_POSITION;
    float4 color : COLOR0;
};

float2 rect_corner(uint vertex_id) {
    if (vertex_id == 0) return float2(0.0, 1.0);
    if (vertex_id == 1) return float2(0.0, 0.0);
    if (vertex_id == 2) return float2(1.0, 1.0);
    return float2(1.0, 0.0);
}

int track_index_from_rect(AnalysisOverlayRect rect) {
    return clamp(int(rect.track_idx & 0xffu), 0, 3);
}

int display_slot_for_track(int track_idx) {
    if (u_order.x == track_idx) return 0;
    if (u_order.y == track_idx) return 1;
    if (u_order.z == track_idx) return 2;
    return 3;
}

float4 overlay_local_rect_from_video_rect(float2 rect_min, float2 rect_max, int track_idx) {
    float2 display_offset = float2(u_display_offset_x[track_idx], u_display_offset_y[track_idx]);
    float2 inv_display_size = float2(u_inv_display_size_x[track_idx], u_inv_display_size_y[track_idx]);
    float2 view_offset_uv = float2(u_view_offset_uv_x[track_idx], u_view_offset_uv_y[track_idx]);
    float2 display_size = float2(
        abs(inv_display_size.x) > 1e-5 ? 1.0 / inv_display_size.x : 0.0,
        abs(inv_display_size.y) > 1e-5 ? 1.0 / inv_display_size.y : 0.0);
    float2 local_min = display_offset + (rect_min + view_offset_uv) * display_size;
    float2 local_max = display_offset + (rect_max + view_offset_uv) * display_size;
    return float4(min(local_min, local_max), max(local_min, local_max));
}

float4 visible_local_rect_for_track(int track_idx) {
    float2 visible_min = float2(0.0, 0.0);
    float2 visible_max = float2(1.0, 1.0);

    if (u_mode == MODE_SPLIT_SCREEN) {
        int slot = display_slot_for_track(track_idx);
        if (slot == 0) {
            visible_max.x = saturate(u_split_pos);
        } else if (slot == 1) {
            visible_min.x = saturate(u_split_pos);
        } else {
            visible_max = visible_min;
        }
    }

    return float4(visible_min, visible_max);
}

float2 overlay_global_uv_from_local_uv(float2 local_uv, int track_idx) {
    if (u_mode == MODE_SPLIT_SCREEN) {
        return local_uv;
    }

    int count = max(u_track_count, 1);
    int slot = clamp(display_slot_for_track(track_idx), 0, count - 1);
    return float2((float(slot) + local_uv.x) / float(count), local_uv.y);
}

float4 unpack_bgra(uint packed) {
    float b = float(packed & 0xffu) / 255.0;
    float g = float((packed >> 8) & 0xffu) / 255.0;
    float r = float((packed >> 16) & 0xffu) / 255.0;
    float a = float((packed >> 24) & 0xffu) / 255.0;
    return float4(r, g, b, a);
}

float2 unpack_uv16(uint packed) {
    return float2(
        float(packed & 0xffffu) / 65535.0,
        float((packed >> 16) & 0xffffu) / 65535.0);
}

VSRectOutput VSMain(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    AnalysisOverlayRect rect = u_analysis_overlay_rects[instance_id];
    int track_idx = track_index_from_rect(rect);
    float2 corner = rect_corner(vertex_id);
    float2 rect_min = unpack_uv16(rect.rect_uv0);
    float2 rect_max = unpack_uv16(rect.rect_uv1);
    float4 local_rect = overlay_local_rect_from_video_rect(rect_min, rect_max, track_idx);
    float4 visible_rect = visible_local_rect_for_track(track_idx);
    float2 clipped_min = max(local_rect.xy, visible_rect.xy);
    float2 clipped_max = min(local_rect.zw, visible_rect.zw);
    if (clipped_max.x <= clipped_min.x || clipped_max.y <= clipped_min.y) {
        clipped_max = clipped_min;
    }

    float2 local_uv = lerp(clipped_min, clipped_max, corner);
    float2 global_uv = overlay_global_uv_from_local_uv(local_uv, track_idx);

    VSRectOutput output;
    output.position = float4(global_uv.x * 2.0 - 1.0, 1.0 - global_uv.y * 2.0, 0.0, 1.0);
    output.color = unpack_bgra(rect.color_bgra);
    return output;
}

float4 PSMain(VSRectOutput input) : SV_TARGET {
    return input.color;
}
