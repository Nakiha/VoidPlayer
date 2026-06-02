R"(
#include <metal_stdlib>
using namespace metal;

	constant int kModeSplitScreen = 1;
	constant uint kMaxTracks = 4;
	constant int kPresentFormatBGRA = 0;
	constant int kPresentFormatNV12 = 1;
	constant int kPresentFormatP010 = 2;
	constant int kPresentFormatYUV420P = 3;
	constant int kColorRangeFull = 2;
	constant int kColorMatrixUnknown = 0;
	constant int kColorMatrixBT601 = 1;
	constant int kColorMatrixBT709 = 2;
	constant int kColorMatrixBT2020NCL = 3;
	constant int kColorTransferSDR = 1;
	constant int kColorTransferPQ = 2;
	constant int kColorTransferHLG = 3;
	constant int kColorPrimariesBT601 = 1;
	constant int kColorPrimariesBT709 = 2;
	constant int kColorPrimariesBT2020 = 3;

struct LayoutParams {
  uint width;
  uint height;
  int mode;
  int track_count;
  float split_pos;
  uint frame_present0;
  uint frame_present1;
  uint frame_present2;
  uint frame_present3;
  int source_width0;
  int source_width1;
  int source_width2;
  int source_width3;
  int source_height0;
	  int source_height1;
	  int source_height2;
	  int source_height3;
	  int yuv_format0;
	  int yuv_format1;
	  int yuv_format2;
	  int yuv_format3;
	  uint y_offset0;
	  uint y_offset1;
	  uint y_offset2;
	  uint y_offset3;
	  uint uv_offset0;
	  uint uv_offset1;
	  uint uv_offset2;
	  uint uv_offset3;
	  uint v_offset0;
	  uint v_offset1;
	  uint v_offset2;
	  uint v_offset3;
	  uint y_stride0;
	  uint y_stride1;
	  uint y_stride2;
	  uint y_stride3;
	  uint uv_stride0;
	  uint uv_stride1;
	  uint uv_stride2;
	  uint uv_stride3;
	  int coded_width0;
	  int coded_width1;
	  int coded_width2;
	  int coded_width3;
	  int coded_height0;
	  int coded_height1;
	  int coded_height2;
	  int coded_height3;
	  float nv12_uv_scale_x0;
	  float nv12_uv_scale_x1;
	  float nv12_uv_scale_x2;
	  float nv12_uv_scale_x3;
	  float nv12_uv_scale_y0;
	  float nv12_uv_scale_y1;
	  float nv12_uv_scale_y2;
	  float nv12_uv_scale_y3;
	  int color_range0;
	  int color_range1;
	  int color_range2;
	  int color_range3;
	  int color_matrix0;
	  int color_matrix1;
	  int color_matrix2;
	  int color_matrix3;
	  int color_transfer0;
	  int color_transfer1;
	  int color_transfer2;
	  int color_transfer3;
	  int color_primaries0;
	  int color_primaries1;
	  int color_primaries2;
	  int color_primaries3;
	  int order0;
	  int order1;
	  int order2;
	  int order3;
  float display_offset_x0;
  float display_offset_x1;
  float display_offset_x2;
  float display_offset_x3;
  float display_offset_y0;
  float display_offset_y1;
  float display_offset_y2;
  float display_offset_y3;
  float inv_display_size_x0;
  float inv_display_size_x1;
  float inv_display_size_x2;
  float inv_display_size_x3;
  float inv_display_size_y0;
  float inv_display_size_y1;
  float inv_display_size_y2;
  float inv_display_size_y3;
  float view_offset_uv_x0;
  float view_offset_uv_x1;
  float view_offset_uv_x2;
  float view_offset_uv_x3;
  float view_offset_uv_y0;
  float view_offset_uv_y1;
  float view_offset_uv_y2;
  float view_offset_uv_y3;
  uint overlay_present0;
  uint overlay_present1;
  uint overlay_present2;
  uint overlay_present3;
};

uint frame_present_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.frame_present0;
  if (index == 1) return params.frame_present1;
  if (index == 2) return params.frame_present2;
  return params.frame_present3;
}

int order_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.order0;
  if (index == 1) return params.order1;
  if (index == 2) return params.order2;
  return params.order3;
}

int source_width_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.source_width0;
  if (index == 1) return params.source_width1;
  if (index == 2) return params.source_width2;
  return params.source_width3;
}

	int source_height_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.source_height0;
	  if (index == 1) return params.source_height1;
	  if (index == 2) return params.source_height2;
	  return params.source_height3;
	}

	int yuv_format_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.yuv_format0;
	  if (index == 1) return params.yuv_format1;
	  if (index == 2) return params.yuv_format2;
	  return params.yuv_format3;
	}

	uint y_offset_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.y_offset0;
	  if (index == 1) return params.y_offset1;
	  if (index == 2) return params.y_offset2;
	  return params.y_offset3;
	}

	uint uv_offset_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.uv_offset0;
	  if (index == 1) return params.uv_offset1;
	  if (index == 2) return params.uv_offset2;
	  return params.uv_offset3;
	}

	uint v_offset_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.v_offset0;
	  if (index == 1) return params.v_offset1;
	  if (index == 2) return params.v_offset2;
	  return params.v_offset3;
	}

	uint y_stride_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.y_stride0;
	  if (index == 1) return params.y_stride1;
	  if (index == 2) return params.y_stride2;
	  return params.y_stride3;
	}

	uint uv_stride_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.uv_stride0;
	  if (index == 1) return params.uv_stride1;
	  if (index == 2) return params.uv_stride2;
	  return params.uv_stride3;
	}

	int coded_width_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.coded_width0;
	  if (index == 1) return params.coded_width1;
	  if (index == 2) return params.coded_width2;
	  return params.coded_width3;
	}

	int coded_height_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.coded_height0;
	  if (index == 1) return params.coded_height1;
	  if (index == 2) return params.coded_height2;
	  return params.coded_height3;
	}

	int color_range_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.color_range0;
	  if (index == 1) return params.color_range1;
	  if (index == 2) return params.color_range2;
	  return params.color_range3;
	}

	int color_matrix_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.color_matrix0;
	  if (index == 1) return params.color_matrix1;
	  if (index == 2) return params.color_matrix2;
	  return params.color_matrix3;
	}

	int color_transfer_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.color_transfer0;
	  if (index == 1) return params.color_transfer1;
	  if (index == 2) return params.color_transfer2;
	  return params.color_transfer3;
	}

	int color_primaries_at(constant LayoutParams& params, uint index) {
	  if (index == 0) return params.color_primaries0;
	  if (index == 1) return params.color_primaries1;
	  if (index == 2) return params.color_primaries2;
	  return params.color_primaries3;
	}

float display_offset_x_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.display_offset_x0;
  if (index == 1) return params.display_offset_x1;
  if (index == 2) return params.display_offset_x2;
  return params.display_offset_x3;
}

float display_offset_y_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.display_offset_y0;
  if (index == 1) return params.display_offset_y1;
  if (index == 2) return params.display_offset_y2;
  return params.display_offset_y3;
}

float inv_display_size_x_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.inv_display_size_x0;
  if (index == 1) return params.inv_display_size_x1;
  if (index == 2) return params.inv_display_size_x2;
  return params.inv_display_size_x3;
}

float inv_display_size_y_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.inv_display_size_y0;
  if (index == 1) return params.inv_display_size_y1;
  if (index == 2) return params.inv_display_size_y2;
  return params.inv_display_size_y3;
}

float view_offset_uv_x_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.view_offset_uv_x0;
  if (index == 1) return params.view_offset_uv_x1;
  if (index == 2) return params.view_offset_uv_x2;
  return params.view_offset_uv_x3;
}

float view_offset_uv_y_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.view_offset_uv_y0;
  if (index == 1) return params.view_offset_uv_y1;
  if (index == 2) return params.view_offset_uv_y2;
  return params.view_offset_uv_y3;
}

uint overlay_present_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.overlay_present0;
  if (index == 1) return params.overlay_present1;
  if (index == 2) return params.overlay_present2;
  return params.overlay_present3;
}

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

	float3 linear_to_srgb(float3 x) {
	  x = max(x, 0.0);
	  float3 lo = x * 12.92;
	  float3 hi = 1.055 * pow(x, 1.0 / 2.4) - 0.055;
	  return mix(lo, hi, step(0.0031308, x));
	}

	float3 srgb_to_linear(float3 x) {
	  x = saturate(x);
	  float3 lo = x / 12.92;
	  float3 hi = pow((x + 0.055) / 1.055, 2.4);
	  return mix(lo, hi, step(0.04045, x));
	}

	float3 convert_linear_primaries_to_bt709(float3 rgb, int primaries) {
	  if (primaries == kColorPrimariesBT2020) {
	    return float3(
	        1.6605 * rgb.r - 0.5876 * rgb.g - 0.0728 * rgb.b,
	       -0.1246 * rgb.r + 1.1329 * rgb.g - 0.0083 * rgb.b,
	       -0.0182 * rgb.r - 0.1006 * rgb.g + 1.1187 * rgb.b);
	  }
	  return rgb;
	}

	float3 pq_to_linear_nits(float3 x) {
	  x = saturate(x);
	  const float m1 = 0.1593017578125;
	  const float m2 = 78.84375;
	  const float c1 = 0.8359375;
	  const float c2 = 18.8515625;
	  const float c3 = 18.6875;
	  float3 p = pow(x, 1.0 / m2);
	  float3 num = max(p - c1, 0.0);
	  float3 den = max(c2 - c3 * p, 1e-6);
	  return pow(num / den, 1.0 / m1) * 10000.0;
	}

	float3 hlg_to_linear(float3 x) {
	  x = saturate(x);
	  const float a = 0.17883277;
	  const float b = 0.28466892;
	  const float c = 0.55991073;
	  float3 lo = (x * x) / 3.0;
	  float3 hi = (exp((x - c) / a) + b) / 12.0;
	  return mix(lo, hi, step(0.5, x));
	}

	float3 tone_map_to_sdr(float3 rgb, int transfer, int primaries) {
	  if (transfer == kColorTransferPQ) {
	    float3 lin = pq_to_linear_nits(rgb) / 203.0;
	    lin = convert_linear_primaries_to_bt709(lin, primaries);
	    return saturate(linear_to_srgb(lin / (1.0 + lin)));
	  }
	  if (transfer == kColorTransferHLG) {
	    float3 lin = hlg_to_linear(rgb) * 4.0;
	    lin = convert_linear_primaries_to_bt709(lin, primaries);
	    return saturate(linear_to_srgb(lin / (1.0 + lin)));
	  }
	  if (primaries == kColorPrimariesBT2020) {
	    float3 lin = convert_linear_primaries_to_bt709(srgb_to_linear(rgb), primaries);
	    return saturate(linear_to_srgb(lin));
	  }
	  return saturate(rgb);
	}

	float yuv_sample_to_float(device const uchar* source, uint offset, bool is_p010) {
	  if (is_p010) {
	    uint lo = uint(source[offset]);
	    uint hi = uint(source[offset + 1]);
	    uint sample = ((hi << 8) | lo) >> 6;
	    return float(sample) / 1023.0;
	  }
	  return float(source[offset]) / 255.0;
	}

	float4 sample_yuv_track(device const uchar* source,
	                        constant LayoutParams& params,
	                        uint track_slot,
	                        uint source_x,
	                        uint source_y) {
	  const int format = yuv_format_at(params, track_slot);
	  const bool is_p010 = format == kPresentFormatP010;
	  const bool is_planar_yuv420 = format == kPresentFormatYUV420P;
	  const uint bytes_per_sample = is_p010 ? 2u : 1u;
	  const uint coded_width = uint(max(coded_width_at(params, track_slot), 1));
	  const uint coded_height = uint(max(coded_height_at(params, track_slot), 1));
	  const uint y_x = min(source_x, coded_width - 1);
	  const uint y_y = min(source_y, coded_height - 1);
	  const uint chroma_width = max((coded_width + 1u) / 2u, 1u);
	  const uint chroma_height = max((coded_height + 1u) / 2u, 1u);
	  const uint uv_x = min(y_x / 2u, chroma_width - 1u);
	  const uint uv_y = min(y_y / 2u, chroma_height - 1u);
	  const uint y_offset =
	      y_offset_at(params, track_slot) + y_y * y_stride_at(params, track_slot) +
	      y_x * bytes_per_sample;
	  const uint uv_offset = uv_offset_at(params, track_slot) +
	      uv_y * uv_stride_at(params, track_slot) +
	      uv_x * bytes_per_sample * (is_planar_yuv420 ? 1u : 2u);
	  const uint v_offset = is_planar_yuv420
	      ? v_offset_at(params, track_slot) +
	            uv_y * uv_stride_at(params, track_slot) +
	            uv_x * bytes_per_sample
	      : uv_offset + bytes_per_sample;
	  const float y = yuv_sample_to_float(source, y_offset, is_p010);
	  const float u = yuv_sample_to_float(source, uv_offset, is_p010);
	  const float v = yuv_sample_to_float(source, v_offset, is_p010);
	  float y_full = y;
	  float2 cbcr = (float2(u, v) * 255.0 - 128.0) / 255.0;
	  if (color_range_at(params, track_slot) != kColorRangeFull) {
	    y_full = (y * 255.0 - 16.0) / 219.0;
	    cbcr = (float2(u, v) * 255.0 - 128.0) / 224.0;
	  }
	  float cb = cbcr.x;
	  float cr = cbcr.y;
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
	      tone_map_to_sdr(rgb,
	                      color_transfer_at(params, track_slot),
	                      color_primaries_at(params, track_slot)),
	      1.0);
	}

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
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    return;
  }
  const int source_width_int = source_width_at(params, track_slot);
  const int source_height_int = source_height_at(params, track_slot);
  if (source_width_int <= 0 || source_height_int <= 0) {
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    return;
  }

  bool out_of_bounds = false;
  const float2 source_uv = aspect_fit_uv(local_uv, params, track_slot, out_of_bounds);
  if (out_of_bounds) {
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    return;
  }

	  const uint source_width = uint(source_width_int);
	  const uint source_height = uint(source_height_int);
	  const uint source_x = min(uint(source_uv.x * float(source_width)), source_width - 1);
	  const uint source_y = min(uint(source_uv.y * float(source_height)), source_height - 1);
	  float4 color = float4(0.0, 0.0, 0.0, 1.0);
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
      tone_map_to_sdr(rgb,
                      color_transfer_at(params, track_slot),
                      color_primaries_at(params, track_slot)),
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
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    return;
  }
  const int source_width_int = source_width_at(params, track_slot);
  const int source_height_int = source_height_at(params, track_slot);
  if (source_width_int <= 0 || source_height_int <= 0) {
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    return;
  }

  bool out_of_bounds = false;
  const float2 source_uv_coord = aspect_fit_uv(texcoord, params, track_slot, out_of_bounds);
  if (out_of_bounds) {
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
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
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    return;
  }
  const int source_width_int = source_width_at(params, track_slot);
  const int source_height_int = source_height_at(params, track_slot);
  if (source_width_int <= 0 || source_height_int <= 0) {
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    return;
  }

  bool out_of_bounds = false;
  const float2 source_uv = aspect_fit_uv(local_uv, params, track_slot, out_of_bounds);
  if (out_of_bounds) {
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    return;
  }

  const uint source_width = uint(source_width_int);
  const uint source_height = uint(source_height_int);
  const uint source_x = min(uint(source_uv.x * float(source_width)), source_width - 1);
  const uint source_y = min(uint(source_uv.y * float(source_height)), source_height - 1);
  float4 color = float4(0.0, 0.0, 0.0, 1.0);
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
  const float2 rect_min = overlay_unpack_uv16(rect.rect_uv0);
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
)";
