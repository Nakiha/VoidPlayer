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
    nointerpolation float4 rect_px : TEXCOORD0;
    nointerpolation float line_strength : TEXCOORD1;
    nointerpolation float4 rect_uv : TEXCOORD2;
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

float line_strength_from_rect(AnalysisOverlayRect rect) {
    return float((rect.track_idx >> 8) & 0xffu) / 255.0;
}

int display_slot_for_track(int track_idx) {
    if (u_order.x == track_idx) return 0;
    if (u_order.y == track_idx) return 1;
    if (u_order.z == track_idx) return 2;
    return 3;
}

float2 unpack_uv16(uint packed) {
    return float2(
        float(packed & 0xffffu) / 65535.0,
        float((packed >> 16) & 0xffffu) / 65535.0);
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

VSMaskOutput VSMain(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    AnalysisOverlayRect rect = u_analysis_overlay_rects[instance_id];
    int track_idx = track_index_from_rect(rect);
    float2 rect_min = unpack_uv16(rect.rect_uv0);
    float2 rect_max = unpack_uv16(rect.rect_uv1);
    float4 local_rect = overlay_local_rect_from_video_rect(rect_min, rect_max, track_idx);
    float4 visible_rect = visible_local_rect_for_track(track_idx);
    float2 clipped_min = max(local_rect.xy, visible_rect.xy);
    float2 clipped_max = min(local_rect.zw, visible_rect.zw);
    if (clipped_max.x <= clipped_min.x || clipped_max.y <= clipped_min.y) {
        clipped_max = clipped_min;
    }

    float2 local_uv = lerp(clipped_min, clipped_max, rect_corner(vertex_id));
    float2 global_uv = overlay_global_uv_from_local_uv(local_uv, track_idx);
    float2 rect_global_min = overlay_global_uv_from_local_uv(local_rect.xy, track_idx);
    float2 rect_global_max = overlay_global_uv_from_local_uv(local_rect.zw, track_idx);

    VSMaskOutput output;
    output.position = float4(global_uv.x * 2.0 - 1.0, 1.0 - global_uv.y * 2.0, 0.0, 1.0);
    output.rect_px = float4(
        rect_global_min.x * u_canvas_width,
        rect_global_min.y * u_canvas_height,
        rect_global_max.x * u_canvas_width,
        rect_global_max.y * u_canvas_height);
    output.line_strength = line_strength_from_rect(rect);
    output.rect_uv = float4(rect_min, rect_max);
    return output;
}

float4 PSMain(VSMaskOutput input) : SV_TARGET {
    if (input.line_strength <= 0.0) {
        discard;
    }

    float2 px = input.position.xy;
    float left = px.x - input.rect_px.x;
    float top = px.y - input.rect_px.y;
    float right = input.rect_px.z - px.x;
    float bottom = input.rect_px.w - px.y;
    const float line_width_px = 0.75;
    bool on_line =
        left <= line_width_px ||
        top <= line_width_px ||
        (input.rect_uv.z >= 0.9999 && right <= line_width_px) ||
        (input.rect_uv.w >= 0.9999 && bottom <= line_width_px);
    if (!on_line) {
        discard;
    }

    return float4(1.0, 1.0, 1.0, 1.0);
}
