float4 sample_cv_yuv_track(texture2d<float, access::read> y_texture,
                           texture2d<float, access::read> uv_texture,
                           constant LayoutParams& params,
                           uint track_slot,
                           uint source_x,
                           uint source_y) {
  const uint coded_width = uint(max(coded_width_at(params, track_slot), 1));
  const uint coded_height = uint(max(coded_height_at(params, track_slot), 1));
  const uint y_x = min(source_x, coded_width - 1);
  const uint y_y = min(source_y, coded_height - 1);
  const uint uv_x = min(y_x / 2u, max(coded_width / 2u, 1u) - 1u);
  const uint uv_y = min(y_y / 2u, max(coded_height / 2u, 1u) - 1u);
  const float y = y_texture.read(uint2(y_x, y_y)).r;
  const float2 uv = uv_texture.read(uint2(uv_x, uv_y)).rg;
  float y_full = y;
  float2 cbcr = (uv * 255.0 - 128.0) / 255.0;
  if (color_range_at(params, track_slot) != kColorRangeFull) {
    y_full = (y * 255.0 - 16.0) / 219.0;
    cbcr = (uv * 255.0 - 128.0) / 224.0;
  }
  const float cb = cbcr.x;
  const float cr = cbcr.y;
  float3 rgb;
  const int matrix = color_matrix_at(params, track_slot);
  if (matrix == kColorMatrixBT2020NCL) {
    rgb = float3(
        y_full + 1.4746 * cr,
        y_full - 0.164553 * cb - 0.571353 * cr,
        y_full + 1.8814 * cb);
  } else if (matrix == kColorMatrixBT709 || matrix == kColorMatrixUnknown) {
    rgb = float3(
        y_full + 1.5748 * cr,
        y_full - 0.187324 * cb - 0.468124 * cr,
        y_full + 1.8556 * cb);
  } else {
    rgb = float3(
        y_full + 1.402 * cr,
        y_full - 0.344136 * cb - 0.714136 * cr,
        y_full + 1.772 * cb);
  }
  if (color_transfer_at(params, track_slot) == kColorTransferSDR) {
    rgb -= (1.0 / 255.0);
  }
  return float4(
      map_to_output(rgb,
                    color_transfer_at(params, track_slot),
                    color_primaries_at(params, track_slot),
                    params.output_edr != 0),
      1.0);
}

kernel void layout_cv_yuv_copy(
    constant LayoutParams& params [[buffer(0)]],
    texture2d<float, access::write> destination [[texture(0)]],
    texture2d<float, access::read> source_y [[texture(1)]],
    texture2d<float, access::read> source_uv [[texture(2)]],
    texture2d<float, access::read> overlay0 [[texture(3)]],
    texture2d<float, access::read> overlay1 [[texture(4)]],
    texture2d<float, access::read> overlay2 [[texture(5)]],
    texture2d<float, access::read> overlay3 [[texture(6)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }

  const float2 canvas_size = float2(float(params.width), float(params.height));
  const float2 texcoord = (float2(gid) + float2(0.5, 0.5)) / canvas_size;
  const uint track_slot = 0u;
  if (frame_present_at(params, track_slot) == 0) {
    destination.write(viewport_background_output_color(params), gid);
    return;
  }
  const int source_width_int = source_width_at(params, track_slot);
  const int source_height_int = source_height_at(params, track_slot);
  if (source_width_int <= 0 || source_height_int <= 0) {
    destination.write(viewport_background_output_color(params), gid);
    return;
  }

  bool out_of_bounds = false;
  const float2 source_uv_coord = aspect_fit_uv(texcoord, params, track_slot, out_of_bounds);
  if (out_of_bounds) {
    destination.write(viewport_background_output_color(params), gid);
    return;
  }

  const uint source_width = uint(source_width_int);
  const uint source_height = uint(source_height_int);
  const uint source_x = min(uint(source_uv_coord.x * float(source_width)), source_width - 1);
  const uint source_y_pos = min(uint(source_uv_coord.y * float(source_height)), source_height - 1);
  float4 color = sample_cv_yuv_track(
      source_y, source_uv, params, track_slot, source_x, source_y_pos);
  color = overlay_blend_over(
      color,
      overlay_sample_for_track(params,
                               track_slot,
                               source_x,
                               source_y_pos,
                               source_uv_coord,
                               float2(gid) + float2(0.5, 0.5),
                               overlay0,
                               overlay1,
                               overlay2,
                               overlay3));
  destination.write(color, gid);
}

kernel void layout_cv_yuv_set_copy(
    constant LayoutParams& params [[buffer(0)]],
    texture2d<float, access::write> destination [[texture(0)]],
    texture2d<float, access::read> source_y0 [[texture(1)]],
    texture2d<float, access::read> source_uv0 [[texture(2)]],
    texture2d<float, access::read> source_y1 [[texture(3)]],
    texture2d<float, access::read> source_uv1 [[texture(4)]],
    texture2d<float, access::read> source_y2 [[texture(5)]],
    texture2d<float, access::read> source_uv2 [[texture(6)]],
    texture2d<float, access::read> source_y3 [[texture(7)]],
    texture2d<float, access::read> source_uv3 [[texture(8)]],
    texture2d<float, access::read> overlay0 [[texture(9)]],
    texture2d<float, access::read> overlay1 [[texture(10)]],
    texture2d<float, access::read> overlay2 [[texture(11)]],
    texture2d<float, access::read> overlay3 [[texture(12)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }

  const float2 canvas_size = float2(float(params.width), float(params.height));
  const float2 texcoord = (float2(gid) + float2(0.5, 0.5)) / canvas_size;
  int track_idx = 0;
  float2 local_uv = texcoord;
  if (params.mode == kModeSplitScreen) {
    track_idx = texcoord.x < params.split_pos
        ? order_at(params, 0)
        : order_at(params, 1);
  } else {
    const int count = max(params.track_count, 1);
    const float scaled_x = texcoord.x * float(count);
    const int display_slot = clamp(int(scaled_x), 0, count - 1);
    track_idx = order_at(params, uint(display_slot));
    local_uv = float2(scaled_x - float(display_slot), texcoord.y);
  }
  track_idx = clamp(track_idx, 0, int(kMaxTracks) - 1);
  const uint track_slot = uint(track_idx);
  if (frame_present_at(params, track_slot) == 0) {
    destination.write(viewport_background_output_color(params), gid);
    return;
  }
  const int source_width_int = source_width_at(params, track_slot);
  const int source_height_int = source_height_at(params, track_slot);
  if (source_width_int <= 0 || source_height_int <= 0) {
    destination.write(viewport_background_output_color(params), gid);
    return;
  }

  bool out_of_bounds = false;
  const float2 source_uv = aspect_fit_uv(local_uv, params, track_slot, out_of_bounds);
  if (out_of_bounds) {
    destination.write(viewport_background_output_color(params), gid);
    return;
  }

  const uint source_width = uint(source_width_int);
  const uint source_height = uint(source_height_int);
  const uint source_x = min(uint(source_uv.x * float(source_width)), source_width - 1);
  const uint source_y = min(uint(source_uv.y * float(source_height)), source_height - 1);
  float4 color = viewport_background_output_color(params);
  if (track_slot == 0u) {
    color = sample_cv_yuv_track(
        source_y0, source_uv0, params, track_slot, source_x, source_y);
  } else if (track_slot == 1u) {
    color = sample_cv_yuv_track(
        source_y1, source_uv1, params, track_slot, source_x, source_y);
  } else if (track_slot == 2u) {
    color = sample_cv_yuv_track(
        source_y2, source_uv2, params, track_slot, source_x, source_y);
  } else {
    color = sample_cv_yuv_track(
        source_y3, source_uv3, params, track_slot, source_x, source_y);
  }

  if (params.mode == kModeSplitScreen && params.width > 0) {
    const float divider_x = params.split_pos * float(params.width);
    const float pixel_x = texcoord.x * float(params.width);
    const float dist = abs(pixel_x - divider_x);
    const float core_width = 1.25;
    const float edge_width = 0.75;
    if (dist <= core_width + edge_width) {
      const float alpha = (dist <= core_width)
          ? 1.0
          : 1.0 - ((dist - core_width) / edge_width);
      const float3 divider_color = 1.0 - color.rgb;
      color.rgb = divider_color * alpha + color.rgb * (1.0 - alpha);
      color.a = 1.0;
    }
  }
  color = overlay_blend_over(
      color,
      overlay_sample_for_track(params,
                               track_slot,
                               source_x,
                               source_y,
                               source_uv,
                               float2(gid) + float2(0.5, 0.5),
                               overlay0,
                               overlay1,
                               overlay2,
                               overlay3));
  destination.write(color, gid);
}
