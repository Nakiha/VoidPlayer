bool overlay_global_rect_from_video_rect(
    OverlayGpuRect rect,
    constant LayoutParams& params,
    thread float4& out_rect) {
  const int track_idx = overlay_track_index_from_rect(rect);
  const float2 rect_min = overlay_unpack_uv16(rect.rect_uv0);
  const float2 rect_max = overlay_unpack_uv16(rect.rect_uv1);
  const float4 local_rect =
      overlay_local_rect_from_video_rect(rect_min, rect_max, params, track_idx);
  const float4 visible_rect = overlay_visible_local_rect_for_track(params, track_idx);
  const float2 clipped_min = max(local_rect.xy, visible_rect.xy);
  const float2 clipped_max = min(local_rect.zw, visible_rect.zw);
  if (clipped_max.x <= clipped_min.x || clipped_max.y <= clipped_min.y) {
    return false;
  }
  const float2 global_min = overlay_global_uv_from_local_uv(clipped_min, params, track_idx);
  const float2 global_max = overlay_global_uv_from_local_uv(clipped_max, params, track_idx);
  out_rect = float4(
      min(global_min.x, global_max.x),
      min(global_min.y, global_max.y),
      max(global_min.x, global_max.x),
      max(global_min.y, global_max.y));
  return true;
}

kernel void composite_overlay_fill_rects(
    device const OverlayGpuRect* rects [[buffer(0)]],
    constant LayoutParams& params [[buffer(1)]],
    texture2d<float, access::read_write> destination [[texture(0)]],
    uint id [[thread_position_in_grid]]) {
  const int width = int(params.width);
  const int height = int(params.height);
  if (width <= 0 || height <= 0) {
    return;
  }
  const OverlayGpuRect rect = rects[id];
  const float4 color = overlay_color_from_bgra(rect.color_bgra);
  if (color.a <= 0.0) {
    return;
  }
  float4 global_rect;
  if (!overlay_global_rect_from_video_rect(rect, params, global_rect)) {
    return;
  }
  const int min_x = clamp(int(floor(global_rect.x * float(width))), 0, width);
  const int max_x = clamp(int(ceil(global_rect.z * float(width))), 0, width);
  const int min_y = clamp(int(floor(global_rect.y * float(height))), 0, height);
  const int max_y = clamp(int(ceil(global_rect.w * float(height))), 0, height);
  for (int y = min_y; y < max_y; ++y) {
    for (int x = min_x; x < max_x; ++x) {
      const uint2 gid = uint2(uint(x), uint(y));
      const float4 dst = destination.read(gid);
      destination.write(overlay_blend_over(dst, color), gid);
    }
  }
}

kernel void composite_overlay_motion_lines(
    device const OverlayGpuRect* lines [[buffer(0)]],
    constant LayoutParams& params [[buffer(1)]],
    texture2d<float, access::read_write> destination [[texture(0)]],
    uint id [[thread_position_in_grid]]) {
  const int width = int(params.width);
  const int height = int(params.height);
  if (width <= 0 || height <= 0) {
    return;
  }
  const OverlayGpuRect line = lines[id];
  const float4 color = overlay_color_from_bgra(line.color_bgra);
  if (color.a <= 0.0) {
    return;
  }
  const int track_idx = overlay_track_index_from_rect(line);
  const float2 p0_video = overlay_unpack_uv16(line.rect_uv0);
  const float2 p1_video = overlay_unpack_uv16(line.rect_uv1);
  const float4 visible_rect = overlay_visible_local_rect_for_track(params, track_idx);
  const float2 p0_local =
      overlay_local_rect_from_video_rect(p0_video, p0_video, params, track_idx).xy;
  const float2 p1_local =
      overlay_local_rect_from_video_rect(p1_video, p1_video, params, track_idx).xy;
  if ((p0_local.x < visible_rect.x && p1_local.x < visible_rect.x) ||
      (p0_local.x > visible_rect.z && p1_local.x > visible_rect.z) ||
      (p0_local.y < visible_rect.y && p1_local.y < visible_rect.y) ||
      (p0_local.y > visible_rect.w && p1_local.y > visible_rect.w)) {
    return;
  }
  const float2 p0_global = overlay_global_uv_from_local_uv(
      clamp(p0_local, visible_rect.xy, visible_rect.zw), params, track_idx);
  const float2 p1_global = overlay_global_uv_from_local_uv(
      clamp(p1_local, visible_rect.xy, visible_rect.zw), params, track_idx);
  int x0 = clamp(int(round(p0_global.x * float(width))), 0, width - 1);
  int y0 = clamp(int(round(p0_global.y * float(height))), 0, height - 1);
  const int x1 = clamp(int(round(p1_global.x * float(width))), 0, width - 1);
  const int y1 = clamp(int(round(p1_global.y * float(height))), 0, height - 1);

  const int dx = abs(x1 - x0);
  const int sx = x0 < x1 ? 1 : -1;
  const int dy = -abs(y1 - y0);
  const int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (int i = 0; i < width + height; ++i) {
    const uint2 gid = uint2(uint(x0), uint(y0));
    const float4 dst = destination.read(gid);
    destination.write(overlay_blend_over(dst, color), gid);
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
    if (x0 < 0 || y0 < 0 || x0 >= width || y0 >= height) {
      break;
    }
  }
}

kernel void build_overlay_line_mask(
    device const OverlayGpuRect* rects [[buffer(0)]],
    device atomic_uint* mask [[buffer(1)]],
    constant LayoutParams& params [[buffer(2)]],
    uint id [[thread_position_in_grid]]) {
  const OverlayGpuRect rect = rects[id];
  const int width = int(params.width);
  const int height = int(params.height);
  if (width <= 0 || height <= 0 || overlay_line_strength_from_rect(rect) <= 0.0) {
    return;
  }

  const int track_idx = overlay_track_index_from_rect(rect);
  const float2 rect_min = overlay_unpack_uv16(rect.rect_uv0);
  const float2 rect_max = overlay_unpack_uv16(rect.rect_uv1);
  const float4 local_rect =
      overlay_local_rect_from_video_rect(rect_min, rect_max, params, track_idx);
  const float4 visible_rect = overlay_visible_local_rect_for_track(params, track_idx);
  const float2 clipped_min = max(local_rect.xy, visible_rect.xy);
  const float2 clipped_max = min(local_rect.zw, visible_rect.zw);
  if (clipped_max.x <= clipped_min.x || clipped_max.y <= clipped_min.y) {
    return;
  }

  const float line_width_px = 2.0;
  const float2 clipped_global_min =
      overlay_global_uv_from_local_uv(clipped_min, params, track_idx);
  const float2 clipped_global_max =
      overlay_global_uv_from_local_uv(clipped_max, params, track_idx);
  const float2 visible_global_min =
      overlay_global_uv_from_local_uv(visible_rect.xy, params, track_idx);
  const float2 visible_global_max =
      overlay_global_uv_from_local_uv(visible_rect.zw, params, track_idx);
  const float2 clipped_px_min =
      min(clipped_global_min, clipped_global_max) * float2(width, height);
  const float2 clipped_px_max =
      max(clipped_global_min, clipped_global_max) * float2(width, height);
  const float2 visible_px_min =
      min(visible_global_min, visible_global_max) * float2(width, height);
  const float2 visible_px_max =
      max(visible_global_min, visible_global_max) * float2(width, height);
  const float2 draw_px_min = max(clipped_px_min - line_width_px, visible_px_min);
  const float2 draw_px_max = min(clipped_px_max + line_width_px, visible_px_max);
  const float2 rect_global_min =
      overlay_global_uv_from_local_uv(local_rect.xy, params, track_idx);
  const float2 rect_global_max =
      overlay_global_uv_from_local_uv(local_rect.zw, params, track_idx);
  const float4 rect_px = float4(
      floor(min(rect_global_min.x, rect_global_max.x) * float(width) + 0.5),
      floor(min(rect_global_min.y, rect_global_max.y) * float(height) + 0.5),
      floor(max(rect_global_min.x, rect_global_max.x) * float(width) + 0.5),
      floor(max(rect_global_min.y, rect_global_max.y) * float(height) + 0.5));
  if (rect_px.z <= rect_px.x || rect_px.w <= rect_px.y) {
    return;
  }

  const int min_x = clamp(int(floor(draw_px_min.x)), 0, width);
  const int max_x = clamp(int(ceil(draw_px_max.x)), 0, width);
  const int min_y = clamp(int(floor(draw_px_min.y)), 0, height);
  const int max_y = clamp(int(ceil(draw_px_max.y)), 0, height);
  for (int y = min_y; y < max_y; ++y) {
    const uint row = uint(y) * params.width;
    for (int x = min_x; x < max_x; ++x) {
      const bool within_y = float(y) >= rect_px.y && float(y) < rect_px.w;
      const bool within_x = float(x) >= rect_px.x && float(x) < rect_px.z;
      const bool on_line =
          (within_y && float(x) >= rect_px.x && float(x) < rect_px.x + line_width_px) ||
          (within_x && float(y) >= rect_px.y && float(y) < rect_px.y + line_width_px) ||
          (rect_max.x >= 0.9999 &&
              within_y && float(x) >= rect_px.z - line_width_px && float(x) < rect_px.z) ||
          (rect_max.y >= 0.9999 &&
              within_x && float(y) >= rect_px.w - line_width_px && float(y) < rect_px.w);
      if (on_line) {
        atomic_store_explicit(&mask[row + uint(x)], 1u, memory_order_relaxed);
      }
    }
  }
}

kernel void composite_overlay_line_contrast(
    device atomic_uint* mask [[buffer(0)]],
    constant LayoutParams& params [[buffer(1)]],
    texture2d<float, access::read_write> destination [[texture(0)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height ||
      gid.x >= destination.get_width() || gid.y >= destination.get_height()) {
    return;
  }

  const int x = int(gid.x);
  const int y = int(gid.y);
  const bool center = overlay_mask_at(mask, params.width, params.height, x, y) > 0;
  const bool halo =
      !center &&
      (overlay_mask_at(mask, params.width, params.height, x - 1, y) > 0 ||
       overlay_mask_at(mask, params.width, params.height, x + 1, y) > 0 ||
       overlay_mask_at(mask, params.width, params.height, x, y - 1) > 0 ||
       overlay_mask_at(mask, params.width, params.height, x, y + 1) > 0);
  if (!center && !halo) {
    return;
  }

  const float4 halo_color = float4(0.0, 0.0, 0.0, 166.0 / 255.0);
  const float4 center_color = float4(1.0, 1.0, 1.0, 230.0 / 255.0);
  const float4 dst = destination.read(gid);
  destination.write(overlay_blend_over(dst, center ? center_color : halo_color), gid);
}
