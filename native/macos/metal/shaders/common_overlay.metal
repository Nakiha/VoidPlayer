struct OverlayGpuRect {
  uint rect_uv0;
  uint rect_uv1;
  uint color_bgra;
  uint track_idx;
};

struct OverlayLayerParams {
  uint width;
  uint height;
  uint track_slot;
  uint reserved0;
};

struct OverlayLinePassParams {
  uint width;
  uint height;
  uint pass;
  uint reserved0;
};

int overlay_track_index_from_rect(OverlayGpuRect rect) {
  return clamp(int(rect.track_idx & 0xffu), 0, 3);
}

float overlay_line_strength_from_rect(OverlayGpuRect rect) {
  return float((rect.track_idx >> 8) & 0xffu) / 255.0;
}

int overlay_display_slot_for_track(constant LayoutParams& params, int track_idx) {
  if (params.order0 == track_idx) return 0;
  if (params.order1 == track_idx) return 1;
  if (params.order2 == track_idx) return 2;
  return 3;
}

float2 overlay_unpack_uv16(uint packed) {
  return float2(
      float(packed & 0xffffu) / 65535.0,
      float((packed >> 16) & 0xffffu) / 65535.0);
}

float4 overlay_local_rect_from_video_rect(
    float2 rect_min,
    float2 rect_max,
    constant LayoutParams& params,
    int track_idx) {
  const uint slot = uint(track_idx);
  const float2 display_offset = float2(
      display_offset_x_at(params, slot),
      display_offset_y_at(params, slot));
  const float2 inv_display_size = float2(
      inv_display_size_x_at(params, slot),
      inv_display_size_y_at(params, slot));
  const float2 view_offset_uv = float2(
      view_offset_uv_x_at(params, slot),
      view_offset_uv_y_at(params, slot));
  const float2 display_size = float2(
      abs(inv_display_size.x) > 1e-5 ? 1.0 / inv_display_size.x : 0.0,
      abs(inv_display_size.y) > 1e-5 ? 1.0 / inv_display_size.y : 0.0);
  const float2 local_min = display_offset + (rect_min + view_offset_uv) * display_size;
  const float2 local_max = display_offset + (rect_max + view_offset_uv) * display_size;
  return float4(min(local_min, local_max), max(local_min, local_max));
}

float4 overlay_visible_local_rect_for_track(constant LayoutParams& params, int track_idx) {
  float2 visible_min = float2(0.0, 0.0);
  float2 visible_max = float2(1.0, 1.0);
  if (params.mode == kModeSplitScreen) {
    const int slot = overlay_display_slot_for_track(params, track_idx);
    if (slot == 0) {
      visible_max.x = clamp(params.split_pos, 0.0, 1.0);
    } else if (slot == 1) {
      visible_min.x = clamp(params.split_pos, 0.0, 1.0);
    } else {
      visible_max = visible_min;
    }
  }
  return float4(visible_min, visible_max);
}

float2 overlay_global_uv_from_local_uv(
    float2 local_uv,
    constant LayoutParams& params,
    int track_idx) {
  if (params.mode == kModeSplitScreen) {
    return local_uv;
  }
  const int count = max(params.track_count, 1);
  const int slot = clamp(overlay_display_slot_for_track(params, track_idx), 0, count - 1);
  return float2((float(slot) + local_uv.x) / float(count), local_uv.y);
}

float4 overlay_color_from_bgra(uint color_bgra) {
  return float4(
      float((color_bgra >> 16) & 0xffu),
      float((color_bgra >> 8) & 0xffu),
      float(color_bgra & 0xffu),
      float((color_bgra >> 24) & 0xffu)) / 255.0;
}

int overlay_snap_line_px(float line_px) {
  return int(floor(line_px + 0.001));
}

