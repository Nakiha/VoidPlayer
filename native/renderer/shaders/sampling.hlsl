// Texture sampling helpers for the supported frame layouts.

float sample_y_plane(int track_idx, float2 uv) {
    if (track_idx == 0) return u_textures_y[0].Sample(u_sampler, uv);
    if (track_idx == 1) return u_textures_y[1].Sample(u_sampler, uv);
    if (track_idx == 2) return u_textures_y[2].Sample(u_sampler, uv);
    return u_textures_y[3].Sample(u_sampler, uv);
}

float2 sample_nv12_uv_plane(int track_idx, float2 uv) {
    if (track_idx == 0) return u_textures_uv[0].Sample(u_sampler, uv);
    if (track_idx == 1) return u_textures_uv[1].Sample(u_sampler, uv);
    if (track_idx == 2) return u_textures_uv[2].Sample(u_sampler, uv);
    return u_textures_uv[3].Sample(u_sampler, uv);
}

float sample_planar_u_plane(int track_idx, float2 uv) {
    if (track_idx == 0) return u_planar_u[0].Sample(u_sampler, uv);
    if (track_idx == 1) return u_planar_u[1].Sample(u_sampler, uv);
    if (track_idx == 2) return u_planar_u[2].Sample(u_sampler, uv);
    return u_planar_u[3].Sample(u_sampler, uv);
}

float sample_planar_v_plane(int track_idx, float2 uv) {
    if (track_idx == 0) return u_planar_v[0].Sample(u_sampler, uv);
    if (track_idx == 1) return u_planar_v[1].Sample(u_sampler, uv);
    if (track_idx == 2) return u_planar_v[2].Sample(u_sampler, uv);
    return u_planar_v[3].Sample(u_sampler, uv);
}

float4 sample_yuv420_track(int track_idx, float2 uv, float y, float2 uv_color) {
    return float4(
        yuv_to_rgb(
            y,
            uv_color,
            u_color_range[track_idx],
            u_color_matrix[track_idx],
            u_color_transfer[track_idx],
            u_color_primaries[track_idx]),
        1.0);
}

float4 sample_track(int track_idx, float2 uv) {
    if (u_nv12_mask & (1 << track_idx)) {
        float2 scaled_uv = float2(
            uv.x * u_nv12_uv_scale_x[track_idx],
            uv.y * u_nv12_uv_scale_y[track_idx]);
        return sample_yuv420_track(
            track_idx,
            scaled_uv,
            sample_y_plane(track_idx, scaled_uv),
            sample_nv12_uv_plane(track_idx, scaled_uv));
    }

    if (u_planar_yuv_mask & (1 << track_idx)) {
        float y = sample_y_plane(track_idx, uv);
        float u = sample_planar_u_plane(track_idx, uv);
        float v = sample_planar_v_plane(track_idx, uv);
        return sample_yuv420_track(track_idx, uv, y, float2(u, v));
    }

    float4 color = float4(0.0, 0.0, 0.0, 1.0);
    if (track_idx == 0)      color = u_textures[0].Sample(u_sampler, uv);
    else if (track_idx == 1) color = u_textures[1].Sample(u_sampler, uv);
    else if (track_idx == 2) color = u_textures[2].Sample(u_sampler, uv);
    else                     color = u_textures[3].Sample(u_sampler, uv);
    if (u_output_target == OUTPUT_TARGET_WINDOWS_SCRGB) {
        color.rgb = map_to_windows_scrgb(
            color.rgb,
            u_color_transfer[track_idx],
            u_color_primaries[track_idx]);
    }
    return color;
}

float2 calc_aspect_fit_uv(
    float2 local_uv,
    int track_idx,
    out bool out_of_bounds
) {
    out_of_bounds = false;

    float2 display_offset = float2(u_display_offset_x[track_idx], u_display_offset_y[track_idx]);
    float2 inv_display_size = float2(u_inv_display_size_x[track_idx], u_inv_display_size_y[track_idx]);
    float2 view_offset_uv = float2(u_view_offset_uv_x[track_idx], u_view_offset_uv_y[track_idx]);

    float2 offset_uv = (local_uv - display_offset) * inv_display_size - view_offset_uv;

    if (offset_uv.x < 0.0 || offset_uv.x > 1.0 ||
        offset_uv.y < 0.0 || offset_uv.y > 1.0) {
        out_of_bounds = true;
        return float2(0.0, 0.0);
    }

    return offset_uv;
}
