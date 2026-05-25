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

	kernel void layout_bgra_copy(
	    device const uchar* source [[buffer(0)]],
	    constant LayoutParams& params [[buffer(1)]],
	    texture2d<float, access::write> destination [[texture(0)]],
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
  destination.write(color, gid);
}
)";
