
	float2 aspect_fit_uv(float2 local_uv,
                     constant LayoutParams& params,
                     uint track_idx,
                     thread bool& out_of_bounds) {
  const float2 display_offset = float2(
      display_offset_x_at(params, track_idx),
      display_offset_y_at(params, track_idx));
  const float2 inv_display_size = float2(
      inv_display_size_x_at(params, track_idx),
      inv_display_size_y_at(params, track_idx));
  const float2 view_offset_uv = float2(
      view_offset_uv_x_at(params, track_idx),
      view_offset_uv_y_at(params, track_idx));
  const float2 source_uv = (local_uv - display_offset) * inv_display_size - view_offset_uv;
  if (source_uv.x < 0.0 || source_uv.x > 1.0 ||
      source_uv.y < 0.0 || source_uv.y > 1.0) {
    out_of_bounds = true;
    return float2(0.0, 0.0);
  }
  out_of_bounds = false;
	  return source_uv;
	}
