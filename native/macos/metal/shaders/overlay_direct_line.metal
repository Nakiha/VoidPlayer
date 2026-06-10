void overlay_write_direct_line_pixel(
    texture2d<float, access::read_write> destination,
    int width,
    int height,
    int x,
    int y,
    float4 color) {
  if (x < 0 || y < 0 || x >= width || y >= height) {
    return;
  }
  const uint2 pos = uint2(uint(x), uint(y));
  const float4 dst = destination.read(pos);
  destination.write(overlay_blend_over(dst, color), pos);
}

void overlay_draw_direct_vertical_line(
    texture2d<float, access::read_write> destination,
    int width,
    int height,
    int x,
    int y0,
    int y1,
    uint pass,
    float4 color) {
  const int min_y = clamp(min(y0, y1), 0, height - 1);
  const int max_y = clamp(max(y0, y1), 0, height - 1);
  if (pass == 0u) {
    for (int y = min_y; y <= max_y; ++y) {
      overlay_write_direct_line_pixel(destination, width, height, x - 1, y, color);
      overlay_write_direct_line_pixel(destination, width, height, x, y, color);
      overlay_write_direct_line_pixel(destination, width, height, x + 1, y, color);
    }
  } else {
    for (int y = min_y; y <= max_y; ++y) {
      overlay_write_direct_line_pixel(destination, width, height, x, y, color);
    }
  }
}

void overlay_draw_direct_horizontal_line(
    texture2d<float, access::read_write> destination,
    int width,
    int height,
    int y,
    int x0,
    int x1,
    uint pass,
    float4 color) {
  const int min_x = clamp(min(x0, x1), 0, width - 1);
  const int max_x = clamp(max(x0, x1), 0, width - 1);
  if (pass == 0u) {
    for (int x = min_x; x <= max_x; ++x) {
      overlay_write_direct_line_pixel(destination, width, height, x, y - 1, color);
      overlay_write_direct_line_pixel(destination, width, height, x, y, color);
      overlay_write_direct_line_pixel(destination, width, height, x, y + 1, color);
    }
  } else {
    for (int x = min_x; x <= max_x; ++x) {
      overlay_write_direct_line_pixel(destination, width, height, x, y, color);
    }
  }
}

kernel void composite_overlay_line_rects_direct(
    device const OverlayGpuRect* rects [[buffer(0)]],
    constant LayoutParams& params [[buffer(1)]],
    constant OverlayLinePassParams& pass_params [[buffer(2)]],
    texture2d<float, access::read_write> destination [[texture(0)]],
    uint id [[thread_position_in_grid]]) {
  const int width = int(pass_params.width);
  const int height = int(pass_params.height);
  if (width <= 0 || height <= 0 || width != int(params.width) ||
      height != int(params.height)) {
    return;
  }
  const OverlayGpuRect rect = rects[id];
  if (overlay_line_strength_from_rect(rect) <= 0.0) {
    return;
  }

  const int track_idx = overlay_track_index_from_rect(rect);
  if (frame_present_at(params, uint(track_idx)) == 0) {
    return;
  }
  const float2 rect_min = overlay_unpack_uv16(rect.rect_uv0);
  const float2 rect_max = overlay_unpack_uv16(rect.rect_uv1);
  const float4 local_rect =
      overlay_local_rect_from_video_rect(rect_min, rect_max, params, track_idx);
  const float4 visible_rect = overlay_visible_local_rect_for_track(params, track_idx);
  const float left = min(local_rect.x, local_rect.z);
  const float right = max(local_rect.x, local_rect.z);
  const float top = min(local_rect.y, local_rect.w);
  const float bottom = max(local_rect.y, local_rect.w);
  const float y0 = max(top, visible_rect.y);
  const float y1 = min(bottom, visible_rect.w);
  const float x0 = max(left, visible_rect.x);
  const float x1 = min(right, visible_rect.z);
  if (x1 <= x0 || y1 <= y0) {
    return;
  }

  const bool draw_left = left >= visible_rect.x && left <= visible_rect.z;
  const bool draw_right =
      max(rect_min.x, rect_max.x) >= 0.9999 && right >= visible_rect.x &&
      right <= visible_rect.z;
  const bool draw_top = top >= visible_rect.y && top <= visible_rect.w;
  const bool draw_bottom =
      max(rect_min.y, rect_max.y) >= 0.9999 && bottom >= visible_rect.y &&
      bottom <= visible_rect.w;
  const float4 color = pass_params.pass == 0u
      ? float4(0.0, 0.0, 0.0, 0.88)
      : float4(1.0, 1.0, 1.0, 0.97);

  if (draw_left) {
    const float2 a = overlay_global_uv_from_local_uv(float2(left, y0), params, track_idx);
    const float2 b = overlay_global_uv_from_local_uv(float2(left, y1), params, track_idx);
    overlay_draw_direct_vertical_line(destination,
                                      width,
                                      height,
                                      overlay_snap_line_px(a.x * float(width)),
                                      int(floor(a.y * float(height))),
                                      int(ceil(b.y * float(height))) - 1,
                                      pass_params.pass,
                                      color);
  }
  if (draw_right) {
    const float2 a = overlay_global_uv_from_local_uv(float2(right, y0), params, track_idx);
    const float2 b = overlay_global_uv_from_local_uv(float2(right, y1), params, track_idx);
    overlay_draw_direct_vertical_line(destination,
                                      width,
                                      height,
                                      overlay_snap_line_px(a.x * float(width)),
                                      int(floor(a.y * float(height))),
                                      int(ceil(b.y * float(height))) - 1,
                                      pass_params.pass,
                                      color);
  }
  if (draw_top) {
    const float2 a = overlay_global_uv_from_local_uv(float2(x0, top), params, track_idx);
    const float2 b = overlay_global_uv_from_local_uv(float2(x1, top), params, track_idx);
    overlay_draw_direct_horizontal_line(destination,
                                        width,
                                        height,
                                        overlay_snap_line_px(a.y * float(height)),
                                        int(floor(a.x * float(width))),
                                        int(ceil(b.x * float(width))) - 1,
                                        pass_params.pass,
                                        color);
  }
  if (draw_bottom) {
    const float2 a = overlay_global_uv_from_local_uv(float2(x0, bottom), params, track_idx);
    const float2 b = overlay_global_uv_from_local_uv(float2(x1, bottom), params, track_idx);
    overlay_draw_direct_horizontal_line(destination,
                                        width,
                                        height,
                                        overlay_snap_line_px(a.y * float(height)),
                                        int(floor(a.x * float(width))),
                                        int(ceil(b.x * float(width))) - 1,
                                        pass_params.pass,
                                        color);
  }
}

