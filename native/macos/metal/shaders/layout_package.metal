	kernel void layout_bgra_copy(
	    device const uchar* source [[buffer(0)]],
	    constant LayoutParams& params [[buffer(1)]],
	    texture2d<float, access::write> destination [[texture(0)]],
    texture2d<float, access::read> overlay0 [[texture(1)]],
    texture2d<float, access::read> overlay1 [[texture(2)]],
    texture2d<float, access::read> overlay2 [[texture(3)]],
    texture2d<float, access::read> overlay3 [[texture(4)]],
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
	  if (yuv_format_at(params, track_slot) == kPresentFormatNV12 ||
	      yuv_format_at(params, track_slot) == kPresentFormatP010 ||
	      yuv_format_at(params, track_slot) == kPresentFormatYUV420P) {
	    color = sample_yuv_track(source, params, track_slot, source_x, source_y);
	  } else {
	    const uint track_offset = track_slot * params.width * params.height * 4u;
	    const uint pixel_offset = track_offset + (source_y * params.width + source_x) * 4u;
	    const uchar b = source[pixel_offset + 0u];
	    const uchar g = source[pixel_offset + 1u];
	    const uchar r = source[pixel_offset + 2u];
	    const uchar a = source[pixel_offset + 3u];
	    color = float4(float(r), float(g), float(b), float(a)) / 255.0;
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
