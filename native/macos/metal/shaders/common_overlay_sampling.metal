
float4 overlay_blend_over(float4 dst, float4 src) {
  return float4(
      src.rgb * src.a + dst.rgb * (1.0 - src.a),
      src.a + dst.a * (1.0 - src.a));
}

float2 overlay_screen_px_per_source_px(
    constant LayoutParams& params,
    uint track_slot,
    uint source_width,
    uint source_height) {
  const float display_slots = float(max(params.track_count, 1));
  const float local_width_px = params.mode == kModeSplitScreen
      ? float(params.width)
      : float(params.width) / display_slots;
  const float local_height_px = float(params.height);
  const float2 inv_display_size = float2(
      abs(inv_display_size_x_at(params, track_slot)),
      abs(inv_display_size_y_at(params, track_slot)));
  return max(float2(local_width_px, local_height_px) /
                 max(inv_display_size * float2(float(source_width), float(source_height)),
                     float2(1.0, 1.0)),
             float2(0.001, 0.001));
}

uint overlay_marker_bits_from_sample(float4 sample) {
  const bool is_marker =
      sample.a > 0.99 && sample.g < 0.01 && sample.r > 0.001;
  if (!is_marker) {
    return 0u;
  }
  return uint(round(saturate(sample.r) * 15.0)) & 15u;
}

uint overlay_marker_bits_at(texture2d<float, access::read> overlay, int x, int y) {
  if (x < 0 || y < 0 ||
      x >= int(overlay.get_width()) || y >= int(overlay.get_height())) {
    return 0u;
  }
  return overlay_marker_bits_from_sample(overlay.read(uint2(uint(x), uint(y))));
}

float overlay_snapped_line_distance_px(float line_screen_px,
                                       float pixel_center_px) {
  // Source-space overlay markers are projected back to screen coordinates for
  // constant-width lines. At some viewport sizes the projected edge lands almost
  // exactly on an integer pixel boundary; tiny inverse-mapping differences can
  // otherwise snap adjacent CU edges to opposite pixels and produce mixed
  // white-only / black-only line styles.
  const float snap_epsilon_px = 0.001;
  const float snapped_line_px = floor(line_screen_px + snap_epsilon_px) + 0.5;
  return abs(pixel_center_px - snapped_line_px);
}

float overlay_line_marker_distance_for_texture(
    texture2d<float, access::read> overlay,
    float2 source_px_coord,
    float2 px_per_source,
    float2 display_px_coord) {
  const int base_x = int(floor(source_px_coord.x));
  const int base_y = int(floor(source_px_coord.y));
  const int radius_x = clamp(int(ceil(1.6 / max(px_per_source.x, 0.001))), 1, 8);
  const int radius_y = clamp(int(ceil(1.6 / max(px_per_source.y, 0.001))), 1, 8);
  float distance_px = 1.0e6;
  for (int dx = -radius_x; dx <= radius_x; ++dx) {
    const int x = base_x + dx;
    const uint bits = overlay_marker_bits_at(overlay, x, base_y);
    if ((bits & 1u) != 0u) {
      const float line_screen_x =
          display_px_coord.x + (float(x) - source_px_coord.x) * px_per_source.x;
      distance_px = min(distance_px,
                        overlay_snapped_line_distance_px(line_screen_x,
                                                         display_px_coord.x));
    }
    if ((bits & 2u) != 0u) {
      const float line_screen_x =
          display_px_coord.x + (float(x + 1) - source_px_coord.x) * px_per_source.x;
      distance_px = min(distance_px,
                        overlay_snapped_line_distance_px(line_screen_x,
                                                         display_px_coord.x));
    }
  }
  for (int dy = -radius_y; dy <= radius_y; ++dy) {
    const int y = base_y + dy;
    const uint bits = overlay_marker_bits_at(overlay, base_x, y);
    if ((bits & 4u) != 0u) {
      const float line_screen_y =
          display_px_coord.y + (float(y) - source_px_coord.y) * px_per_source.y;
      distance_px = min(distance_px,
                        overlay_snapped_line_distance_px(line_screen_y,
                                                         display_px_coord.y));
    }
    if ((bits & 8u) != 0u) {
      const float line_screen_y =
          display_px_coord.y + (float(y + 1) - source_px_coord.y) * px_per_source.y;
      distance_px = min(distance_px,
                        overlay_snapped_line_distance_px(line_screen_y,
                                                         display_px_coord.y));
    }
  }
  return distance_px;
}

float4 overlay_decode_line_marker(float4 base_overlay, float distance_px) {
  if (distance_px < 0.55) {
    return overlay_blend_over(base_overlay, float4(1.0, 1.0, 1.0, 0.95));
  }
  if (distance_px < 1.55) {
    return overlay_blend_over(base_overlay, float4(0.0, 0.0, 0.0, 0.85));
  }
  return base_overlay;
}

float4 overlay_sample_for_track(
    constant LayoutParams& params,
    uint track_slot,
    uint source_x,
    uint source_y,
    float2 source_uv,
    float2 display_px_coord,
    texture2d<float, access::read> overlay0,
    texture2d<float, access::read> overlay1,
    texture2d<float, access::read> overlay2,
    texture2d<float, access::read> overlay3) {
  if (overlay_present_at(params, track_slot) == 0) {
    return float4(0.0, 0.0, 0.0, 0.0);
  }
  float4 sample = float4(0.0, 0.0, 0.0, 0.0);
  if (track_slot == 0u) {
    sample = overlay0.read(uint2(min(source_x, overlay0.get_width() - 1),
                                 min(source_y, overlay0.get_height() - 1)));
  } else if (track_slot == 1u) {
    sample = overlay1.read(uint2(min(source_x, overlay1.get_width() - 1),
                                 min(source_y, overlay1.get_height() - 1)));
  } else if (track_slot == 2u) {
    sample = overlay2.read(uint2(min(source_x, overlay2.get_width() - 1),
                                 min(source_y, overlay2.get_height() - 1)));
  } else {
    sample = overlay3.read(uint2(min(source_x, overlay3.get_width() - 1),
                                 min(source_y, overlay3.get_height() - 1)));
  }

  const uint center_bits = overlay_marker_bits_from_sample(sample);
  return center_bits == 0u ? sample : float4(0.0, 0.0, 0.0, 0.0);
}

