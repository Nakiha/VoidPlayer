bool overlay_rect_matches_layer(OverlayGpuRect rect, constant OverlayLayerParams& params) {
  return uint(overlay_track_index_from_rect(rect)) == params.track_slot;
}

float4 overlay_video_px_rect(OverlayGpuRect rect, constant OverlayLayerParams& params) {
  const float2 rect_min = overlay_unpack_uv16(rect.rect_uv0);
  const float2 rect_max = overlay_unpack_uv16(rect.rect_uv1);
  const float2 scale = float2(float(params.width), float(params.height));
  const float2 px_min = min(rect_min, rect_max) * scale;
  const float2 px_max = max(rect_min, rect_max) * scale;
  return float4(px_min, px_max);
}

kernel void clear_overlay_layer(
    texture2d<float, access::write> layer [[texture(0)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= layer.get_width() || gid.y >= layer.get_height()) {
    return;
  }
  layer.write(float4(0.0, 0.0, 0.0, 0.0), gid);
}

kernel void raster_overlay_fill_rects_layer(
    device const OverlayGpuRect* rects [[buffer(0)]],
    constant OverlayLayerParams& params [[buffer(1)]],
    texture2d<float, access::read_write> layer [[texture(0)]],
    uint id [[thread_position_in_grid]]) {
  if (!overlay_rect_matches_layer(rects[id], params)) {
    return;
  }
  const float4 color = overlay_color_from_bgra(rects[id].color_bgra);
  if (color.a <= 0.0) {
    return;
  }
  const float4 rect_px = overlay_video_px_rect(rects[id], params);
  const int min_x = clamp(int(floor(rect_px.x)), 0, int(params.width));
  const int max_x = clamp(int(ceil(rect_px.z)), 0, int(params.width));
  const int min_y = clamp(int(floor(rect_px.y)), 0, int(params.height));
  const int max_y = clamp(int(ceil(rect_px.w)), 0, int(params.height));
  for (int y = min_y; y < max_y; ++y) {
    for (int x = min_x; x < max_x; ++x) {
      const uint2 pos = uint2(uint(x), uint(y));
      const float4 dst = layer.read(pos);
      layer.write(overlay_blend_over(dst, color), pos);
    }
  }
}

struct OverlayFillRectVertexOut {
  float4 position [[position]];
  float4 color;
};

vertex OverlayFillRectVertexOut overlay_fill_rect_layer_vertex(
    device const OverlayGpuRect* rects [[buffer(0)]],
    constant OverlayLayerParams& params [[buffer(1)]],
    uint vertex_id [[vertex_id]]) {
  OverlayFillRectVertexOut out;
  out.position = float4(-2.0, -2.0, 0.0, 1.0);
  out.color = float4(0.0, 0.0, 0.0, 0.0);
  if (params.width == 0 || params.height == 0) {
    return out;
  }

  const uint rect_id = vertex_id / 6u;
  const uint corner = vertex_id - rect_id * 6u;
  const OverlayGpuRect rect = rects[rect_id];
  if (!overlay_rect_matches_layer(rect, params)) {
    return out;
  }
  const float4 color = overlay_color_from_bgra(rect.color_bgra);
  if (color.a <= 0.0) {
    return out;
  }

  const float4 rect_px = overlay_video_px_rect(rect, params);
  const float2 min_px = clamp(floor(rect_px.xy), float2(0.0), float2(params.width, params.height));
  const float2 max_px = clamp(ceil(rect_px.zw), float2(0.0), float2(params.width, params.height));
  if (max_px.x <= min_px.x || max_px.y <= min_px.y) {
    return out;
  }

  float2 px = min_px;
  if (corner == 1u || corner == 3u || corner == 4u) {
    px.x = max_px.x;
  }
  if (corner == 2u || corner == 4u || corner == 5u) {
    px.y = max_px.y;
  }
  out.position = float4(
      px.x / float(params.width) * 2.0 - 1.0,
      1.0 - px.y / float(params.height) * 2.0,
      0.0,
      1.0);
  out.color = color;
  return out;
}

fragment float4 overlay_fill_rect_layer_fragment(
    OverlayFillRectVertexOut in [[stage_in]]) {
  return in.color;
}

kernel void raster_overlay_motion_lines_layer(
    device const OverlayGpuRect* lines [[buffer(0)]],
    constant OverlayLayerParams& params [[buffer(1)]],
    texture2d<float, access::read_write> layer [[texture(0)]],
    uint id [[thread_position_in_grid]]) {
  if (!overlay_rect_matches_layer(lines[id], params)) {
    return;
  }
  const float4 color = overlay_color_from_bgra(lines[id].color_bgra);
  if (color.a <= 0.0 || params.width == 0 || params.height == 0) {
    return;
  }
  const float2 p0_uv = overlay_unpack_uv16(lines[id].rect_uv0);
  const float2 p1_uv = overlay_unpack_uv16(lines[id].rect_uv1);
  int x0 = clamp(int(round(p0_uv.x * float(params.width))), 0, int(params.width) - 1);
  int y0 = clamp(int(round(p0_uv.y * float(params.height))), 0, int(params.height) - 1);
  const int x1 = clamp(int(round(p1_uv.x * float(params.width))), 0, int(params.width) - 1);
  const int y1 = clamp(int(round(p1_uv.y * float(params.height))), 0, int(params.height) - 1);
  const int dx = abs(x1 - x0);
  const int sx = x0 < x1 ? 1 : -1;
  const int dy = -abs(y1 - y0);
  const int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int i = 0; i < int(params.width + params.height); ++i) {
    const uint2 pos = uint2(uint(x0), uint(y0));
    const float4 dst = layer.read(pos);
    layer.write(overlay_blend_over(dst, color), pos);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
    if (x0 < 0 || y0 < 0 || x0 >= int(params.width) || y0 >= int(params.height)) {
      break;
    }
  }
}

kernel void build_overlay_line_mask_layer(
    device const OverlayGpuRect* rects [[buffer(0)]],
    device atomic_uint* mask [[buffer(1)]],
    constant OverlayLayerParams& params [[buffer(2)]],
    uint id [[thread_position_in_grid]]) {
  const OverlayGpuRect rect = rects[id];
  if (!overlay_rect_matches_layer(rect, params) ||
      params.width == 0 || params.height == 0 ||
      overlay_line_strength_from_rect(rect) <= 0.0) {
    return;
  }
  const float4 rect_px = overlay_video_px_rect(rect, params);
  const float4 snapped_rect_px = float4(
      floor(rect_px.x + 0.5),
      floor(rect_px.y + 0.5),
      floor(rect_px.z + 0.5),
      floor(rect_px.w + 0.5));
  if (snapped_rect_px.z <= snapped_rect_px.x ||
      snapped_rect_px.w <= snapped_rect_px.y) {
    return;
  }
  const float line_width_px = 1.0;
  const int min_x =
      clamp(int(floor(snapped_rect_px.x - line_width_px)), 0, int(params.width));
  const int max_x =
      clamp(int(ceil(snapped_rect_px.z + line_width_px)), 0, int(params.width));
  const int min_y =
      clamp(int(floor(snapped_rect_px.y - line_width_px)), 0, int(params.height));
  const int max_y =
      clamp(int(ceil(snapped_rect_px.w + line_width_px)), 0, int(params.height));
  const float2 rect_max = overlay_unpack_uv16(rect.rect_uv1);
  for (int y = min_y; y < max_y; ++y) {
    const uint row = uint(y) * params.width;
    for (int x = min_x; x < max_x; ++x) {
      const bool within_y =
          float(y) >= snapped_rect_px.y && float(y) < snapped_rect_px.w;
      const bool within_x =
          float(x) >= snapped_rect_px.x && float(x) < snapped_rect_px.z;
      uint edge_bits = 0u;
      if (within_y && float(x) >= snapped_rect_px.x &&
          float(x) < snapped_rect_px.x + line_width_px) {
        edge_bits |= 1u;
      }
      if (rect_max.x >= 0.9999 &&
          within_y && float(x) >= snapped_rect_px.z - line_width_px &&
          float(x) < snapped_rect_px.z) {
        edge_bits |= 2u;
      }
      if (within_x && float(y) >= snapped_rect_px.y &&
          float(y) < snapped_rect_px.y + line_width_px) {
        edge_bits |= 4u;
      }
      if (rect_max.y >= 0.9999 &&
          within_x && float(y) >= snapped_rect_px.w - line_width_px &&
          float(y) < snapped_rect_px.w) {
        edge_bits |= 8u;
      }
      if (edge_bits != 0u) {
        atomic_fetch_or_explicit(&mask[row + uint(x)], edge_bits, memory_order_relaxed);
      }
    }
  }
}

uint overlay_mask_at(device atomic_uint* mask, uint width, uint height, int x, int y) {
  if (x < 0 || y < 0 || x >= int(width) || y >= int(height)) {
    return 0u;
  }
  return atomic_load_explicit(&mask[uint(y) * width + uint(x)], memory_order_relaxed);
}

kernel void composite_overlay_line_layer(
    device atomic_uint* mask [[buffer(0)]],
    constant OverlayLayerParams& params [[buffer(1)]],
    texture2d<float, access::read_write> layer [[texture(0)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height ||
      gid.x >= layer.get_width() || gid.y >= layer.get_height()) {
    return;
  }
  const int x = int(gid.x);
  const int y = int(gid.y);
  const uint center_bits = overlay_mask_at(mask, params.width, params.height, x, y) & 15u;
  if (center_bits == 0u) {
    return;
  }
  const float marker = float(center_bits) / 15.0;
  layer.write(float4(marker, 0.0, 0.0, 1.0), gid);
}
