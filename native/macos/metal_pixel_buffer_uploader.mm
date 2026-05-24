#include "native_player_bridge.h"

#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {

constexpr const char* kLayoutBgraKernelSource = R"(
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

struct MetalLayoutParams {
  uint32_t width;
  uint32_t height;
  int32_t mode;
  int32_t track_count;
  float split_pos;
  uint32_t frame_present0;
  uint32_t frame_present1;
  uint32_t frame_present2;
  uint32_t frame_present3;
  int32_t source_width0;
  int32_t source_width1;
  int32_t source_width2;
  int32_t source_width3;
  int32_t source_height0;
  int32_t source_height1;
  int32_t source_height2;
  int32_t source_height3;
  int32_t yuv_format0;
  int32_t yuv_format1;
  int32_t yuv_format2;
  int32_t yuv_format3;
  uint32_t y_offset0;
  uint32_t y_offset1;
  uint32_t y_offset2;
  uint32_t y_offset3;
  uint32_t uv_offset0;
  uint32_t uv_offset1;
  uint32_t uv_offset2;
  uint32_t uv_offset3;
  uint32_t v_offset0;
  uint32_t v_offset1;
  uint32_t v_offset2;
  uint32_t v_offset3;
  uint32_t y_stride0;
  uint32_t y_stride1;
  uint32_t y_stride2;
  uint32_t y_stride3;
  uint32_t uv_stride0;
  uint32_t uv_stride1;
  uint32_t uv_stride2;
  uint32_t uv_stride3;
  int32_t coded_width0;
  int32_t coded_width1;
  int32_t coded_width2;
  int32_t coded_width3;
  int32_t coded_height0;
  int32_t coded_height1;
  int32_t coded_height2;
  int32_t coded_height3;
  float nv12_uv_scale_x0;
  float nv12_uv_scale_x1;
  float nv12_uv_scale_x2;
  float nv12_uv_scale_x3;
  float nv12_uv_scale_y0;
  float nv12_uv_scale_y1;
  float nv12_uv_scale_y2;
  float nv12_uv_scale_y3;
  int32_t color_range0;
  int32_t color_range1;
  int32_t color_range2;
  int32_t color_range3;
  int32_t color_matrix0;
  int32_t color_matrix1;
  int32_t color_matrix2;
  int32_t color_matrix3;
  int32_t color_transfer0;
  int32_t color_transfer1;
  int32_t color_transfer2;
  int32_t color_transfer3;
  int32_t color_primaries0;
  int32_t color_primaries1;
  int32_t color_primaries2;
  int32_t color_primaries3;
  int32_t order0;
  int32_t order1;
  int32_t order2;
  int32_t order3;
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

void write_error(char* error, size_t error_size, const char* message) {
  if (!error || error_size == 0) {
    return;
  }
  const size_t length = message ? std::strlen(message) : 0;
  const size_t copy_size = std::min(error_size - 1, length);
  if (copy_size > 0) {
    std::memcpy(error, message, copy_size);
  }
  error[copy_size] = '\0';
}

bool checked_mul_size(size_t lhs, size_t rhs, size_t* out) {
  if (!out) {
    return false;
  }
  if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
    return false;
  }
  *out = lhs * rhs;
  return true;
}

int metal_upload_failure(char* error, size_t error_size, const char* message) {
  write_error(error, error_size, message);
  return -2;
}

int64_t elapsed_us_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

const char* metal_uploader_status_message(int status) {
  switch (status) {
  case VPMacOSMetalUploaderStatusOk:
    return "";
  case VPMacOSMetalUploaderStatusUnavailable:
    return "native Metal uploader is not available";
  case VPMacOSMetalUploaderStatusInvalidArguments:
    return "invalid native Metal pixel buffer validation arguments";
  case VPMacOSMetalUploaderStatusSizeMismatch:
    return "native Metal pixel buffer dimensions do not match the presentation surface";
  case VPMacOSMetalUploaderStatusUnsupportedPixelFormat:
    return "native Metal pixel buffer must be 32-bit BGRA";
  case VPMacOSMetalUploaderStatusTextureWrapFailed:
    return "failed to wrap CVPixelBuffer as a Metal BGRA texture";
  default:
    return "unknown native Metal pixel buffer validation failure";
  }
}

void write_first_present_frame_info(const VPMacOSNativePresentDecisionInfo& decisionInfo,
                                    VPMacOSNativeFrameInfo* out) {
  if (!out) {
    return;
  }
  *out = {};
  for (int slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    if (decisionInfo.frames[slot].present) {
      out->width = decisionInfo.frames[slot].width;
      out->height = decisionInfo.frames[slot].height;
      out->pts_us = decisionInfo.frames[slot].pts_us;
      out->dts_us = decisionInfo.frames[slot].dts_us;
      out->duration_us = decisionInfo.frames[slot].duration_us;
      break;
    }
  }
}

void fill_metal_layout_params(MetalLayoutParams& metalParams,
                              const VPMacOSNativePresentDecisionInfo& decisionInfo,
                              int32_t width,
                              int32_t height) {
  metalParams.width = static_cast<uint32_t>(width);
  metalParams.height = static_cast<uint32_t>(height);
  metalParams.mode = decisionInfo.mode;
  metalParams.track_count = decisionInfo.track_count;
  metalParams.split_pos = decisionInfo.split_pos;
  metalParams.frame_present0 =
      static_cast<uint32_t>(decisionInfo.frames[0].present ? 1u : 0u);
  metalParams.frame_present1 =
      static_cast<uint32_t>(decisionInfo.frames[1].present ? 1u : 0u);
  metalParams.frame_present2 =
      static_cast<uint32_t>(decisionInfo.frames[2].present ? 1u : 0u);
  metalParams.frame_present3 =
      static_cast<uint32_t>(decisionInfo.frames[3].present ? 1u : 0u);
  metalParams.source_width0 = decisionInfo.source_width[0];
  metalParams.source_width1 = decisionInfo.source_width[1];
  metalParams.source_width2 = decisionInfo.source_width[2];
  metalParams.source_width3 = decisionInfo.source_width[3];
  metalParams.source_height0 = decisionInfo.source_height[0];
  metalParams.source_height1 = decisionInfo.source_height[1];
  metalParams.source_height2 = decisionInfo.source_height[2];
  metalParams.source_height3 = decisionInfo.source_height[3];
  metalParams.yuv_format0 = decisionInfo.yuv_format[0];
  metalParams.yuv_format1 = decisionInfo.yuv_format[1];
  metalParams.yuv_format2 = decisionInfo.yuv_format[2];
  metalParams.yuv_format3 = decisionInfo.yuv_format[3];
  metalParams.y_offset0 = static_cast<uint32_t>(std::max(0, decisionInfo.y_offset[0]));
  metalParams.y_offset1 = static_cast<uint32_t>(std::max(0, decisionInfo.y_offset[1]));
  metalParams.y_offset2 = static_cast<uint32_t>(std::max(0, decisionInfo.y_offset[2]));
  metalParams.y_offset3 = static_cast<uint32_t>(std::max(0, decisionInfo.y_offset[3]));
  metalParams.uv_offset0 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_offset[0]));
  metalParams.uv_offset1 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_offset[1]));
  metalParams.uv_offset2 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_offset[2]));
  metalParams.uv_offset3 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_offset[3]));
  metalParams.v_offset0 = static_cast<uint32_t>(std::max(0, decisionInfo.v_offset[0]));
  metalParams.v_offset1 = static_cast<uint32_t>(std::max(0, decisionInfo.v_offset[1]));
  metalParams.v_offset2 = static_cast<uint32_t>(std::max(0, decisionInfo.v_offset[2]));
  metalParams.v_offset3 = static_cast<uint32_t>(std::max(0, decisionInfo.v_offset[3]));
  metalParams.y_stride0 = static_cast<uint32_t>(std::max(0, decisionInfo.y_stride[0]));
  metalParams.y_stride1 = static_cast<uint32_t>(std::max(0, decisionInfo.y_stride[1]));
  metalParams.y_stride2 = static_cast<uint32_t>(std::max(0, decisionInfo.y_stride[2]));
  metalParams.y_stride3 = static_cast<uint32_t>(std::max(0, decisionInfo.y_stride[3]));
  metalParams.uv_stride0 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_stride[0]));
  metalParams.uv_stride1 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_stride[1]));
  metalParams.uv_stride2 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_stride[2]));
  metalParams.uv_stride3 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_stride[3]));
  metalParams.coded_width0 = decisionInfo.coded_width[0];
  metalParams.coded_width1 = decisionInfo.coded_width[1];
  metalParams.coded_width2 = decisionInfo.coded_width[2];
  metalParams.coded_width3 = decisionInfo.coded_width[3];
  metalParams.coded_height0 = decisionInfo.coded_height[0];
  metalParams.coded_height1 = decisionInfo.coded_height[1];
  metalParams.coded_height2 = decisionInfo.coded_height[2];
  metalParams.coded_height3 = decisionInfo.coded_height[3];
  metalParams.nv12_uv_scale_x0 = decisionInfo.nv12_uv_scale_x[0];
  metalParams.nv12_uv_scale_x1 = decisionInfo.nv12_uv_scale_x[1];
  metalParams.nv12_uv_scale_x2 = decisionInfo.nv12_uv_scale_x[2];
  metalParams.nv12_uv_scale_x3 = decisionInfo.nv12_uv_scale_x[3];
  metalParams.nv12_uv_scale_y0 = decisionInfo.nv12_uv_scale_y[0];
  metalParams.nv12_uv_scale_y1 = decisionInfo.nv12_uv_scale_y[1];
  metalParams.nv12_uv_scale_y2 = decisionInfo.nv12_uv_scale_y[2];
  metalParams.nv12_uv_scale_y3 = decisionInfo.nv12_uv_scale_y[3];
  metalParams.color_range0 = decisionInfo.color_range[0];
  metalParams.color_range1 = decisionInfo.color_range[1];
  metalParams.color_range2 = decisionInfo.color_range[2];
  metalParams.color_range3 = decisionInfo.color_range[3];
  metalParams.color_matrix0 = decisionInfo.color_matrix[0];
  metalParams.color_matrix1 = decisionInfo.color_matrix[1];
  metalParams.color_matrix2 = decisionInfo.color_matrix[2];
  metalParams.color_matrix3 = decisionInfo.color_matrix[3];
  metalParams.color_transfer0 = decisionInfo.color_transfer[0];
  metalParams.color_transfer1 = decisionInfo.color_transfer[1];
  metalParams.color_transfer2 = decisionInfo.color_transfer[2];
  metalParams.color_transfer3 = decisionInfo.color_transfer[3];
  metalParams.color_primaries0 = decisionInfo.color_primaries[0];
  metalParams.color_primaries1 = decisionInfo.color_primaries[1];
  metalParams.color_primaries2 = decisionInfo.color_primaries[2];
  metalParams.color_primaries3 = decisionInfo.color_primaries[3];
  metalParams.order0 = decisionInfo.order[0];
  metalParams.order1 = decisionInfo.order[1];
  metalParams.order2 = decisionInfo.order[2];
  metalParams.order3 = decisionInfo.order[3];
  metalParams.display_offset_x0 = decisionInfo.display_offset_x[0];
  metalParams.display_offset_x1 = decisionInfo.display_offset_x[1];
  metalParams.display_offset_x2 = decisionInfo.display_offset_x[2];
  metalParams.display_offset_x3 = decisionInfo.display_offset_x[3];
  metalParams.display_offset_y0 = decisionInfo.display_offset_y[0];
  metalParams.display_offset_y1 = decisionInfo.display_offset_y[1];
  metalParams.display_offset_y2 = decisionInfo.display_offset_y[2];
  metalParams.display_offset_y3 = decisionInfo.display_offset_y[3];
  metalParams.inv_display_size_x0 = decisionInfo.inv_display_size_x[0];
  metalParams.inv_display_size_x1 = decisionInfo.inv_display_size_x[1];
  metalParams.inv_display_size_x2 = decisionInfo.inv_display_size_x[2];
  metalParams.inv_display_size_x3 = decisionInfo.inv_display_size_x[3];
  metalParams.inv_display_size_y0 = decisionInfo.inv_display_size_y[0];
  metalParams.inv_display_size_y1 = decisionInfo.inv_display_size_y[1];
  metalParams.inv_display_size_y2 = decisionInfo.inv_display_size_y[2];
  metalParams.inv_display_size_y3 = decisionInfo.inv_display_size_y[3];
  metalParams.view_offset_uv_x0 = decisionInfo.view_offset_uv_x[0];
  metalParams.view_offset_uv_x1 = decisionInfo.view_offset_uv_x[1];
  metalParams.view_offset_uv_x2 = decisionInfo.view_offset_uv_x[2];
  metalParams.view_offset_uv_x3 = decisionInfo.view_offset_uv_x[3];
  metalParams.view_offset_uv_y0 = decisionInfo.view_offset_uv_y[0];
  metalParams.view_offset_uv_y1 = decisionInfo.view_offset_uv_y[1];
  metalParams.view_offset_uv_y2 = decisionInfo.view_offset_uv_y[2];
  metalParams.view_offset_uv_y3 = decisionInfo.view_offset_uv_y[3];
}

}  // namespace

@interface VPMacOSMetalUploaderImpl : NSObject {
 @private
  id<MTLDevice> _device;
  id<MTLCommandQueue> _commandQueue;
  id<MTLBuffer> _stagingBuffer;
  id<MTLBuffer> _layoutParamsBuffer;
  id<MTLComputePipelineState> _layoutPipeline;
  id<MTLComputePipelineState> _cvPixelBufferPipeline;
  CVMetalTextureCacheRef _textureCache;
  std::atomic<int64_t> _directYuvUploadCount;
  std::atomic<int64_t> _cvPixelBufferUploadCount;
  std::atomic<int64_t> _presentPackageUploadCount;
  std::atomic<int64_t> _lastPresentPackageCopyUs;
  std::atomic<int64_t> _lastPresentPackageGpuWaitUs;
  std::atomic<int64_t> _lastPresentPackageTotalUs;
  std::atomic<int32_t> _lastPresentPackageStorage;
}

- (BOOL)isAvailable;
- (int64_t)directYuvUploadCount;
- (int64_t)cvPixelBufferUploadCount;
- (int64_t)presentPackageUploadCount;
- (int64_t)lastPresentPackageCopyUs;
- (int64_t)lastPresentPackageGpuWaitUs;
- (int64_t)lastPresentPackageTotalUs;
- (int32_t)lastPresentPackageStorage;
- (int)validatePixelBufferStatus:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height;
- (BOOL)validatePixelBuffer:(CVPixelBufferRef)pixelBuffer
                      width:(int32_t)width
                     height:(int32_t)height;
- (int)copyCurrentFrameFromPlayer:(VPMacOSNativePlayer*)player
                    toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height
                    waitTimeoutMs:(int32_t)waitTimeoutMs
                              out:(VPMacOSNativeFrameInfo*)out
                            error:(char*)error
                        errorSize:(size_t)errorSize;
- (int)copyCurrentFrameWithLayoutFromPlayer:(VPMacOSNativePlayer*)player
                              toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                      width:(int32_t)width
                                     height:(int32_t)height
                              maxTrackSlots:(int32_t)maxTrackSlots
                              waitTimeoutMs:(int32_t)waitTimeoutMs
                                        out:(VPMacOSNativeFrameInfo*)out
                                      error:(char*)error
                                  errorSize:(size_t)errorSize;
- (int)copyPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           data:(const uint8_t*)data
                       dataSize:(size_t)dataSize
                  toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                          width:(int32_t)width
                         height:(int32_t)height
                            out:(VPMacOSNativeFrameInfo*)out
                          error:(char*)error
                      errorSize:(size_t)errorSize;
- (int)copyCVPixelBufferPresentFrame:(const VPMacOSNativeCVPixelBufferPresentFrame*)frame
                        toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                width:(int32_t)width
                               height:(int32_t)height
                                  out:(VPMacOSNativeFrameInfo*)out
                                error:(char*)error
                            errorSize:(size_t)errorSize;
- (int)uploadPreparedPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                   width:(int32_t)width
                                  height:(int32_t)height
                                     out:(VPMacOSNativeFrameInfo*)out
                                   error:(char*)error
                               errorSize:(size_t)errorSize;

@end

@implementation VPMacOSMetalUploaderImpl

- (instancetype)init {
  self = [super init];
  if (self) {
    _directYuvUploadCount.store(0, std::memory_order_relaxed);
    _cvPixelBufferUploadCount.store(0, std::memory_order_relaxed);
    _presentPackageUploadCount.store(0, std::memory_order_relaxed);
    _lastPresentPackageCopyUs.store(0, std::memory_order_relaxed);
    _lastPresentPackageGpuWaitUs.store(0, std::memory_order_relaxed);
    _lastPresentPackageTotalUs.store(0, std::memory_order_relaxed);
    _lastPresentPackageStorage.store(
        VPMacOSNativePresentPackageStorageUnavailable,
        std::memory_order_relaxed);
    _device = MTLCreateSystemDefaultDevice();
    if (_device) {
      _commandQueue = [_device newCommandQueue];
      CVMetalTextureCacheRef cache = nullptr;
      if (CVMetalTextureCacheCreate(
              kCFAllocatorDefault, nullptr, _device, nullptr, &cache) ==
          kCVReturnSuccess) {
        _textureCache = cache;
      }
      NSError* libraryError = nil;
      NSString* source =
          [[NSString alloc] initWithUTF8String:kLayoutBgraKernelSource];
      id<MTLLibrary> library = [_device newLibraryWithSource:source
                                                     options:nil
                                                       error:&libraryError];
      id<MTLFunction> function =
          library ? [library newFunctionWithName:@"layout_bgra_copy"] : nil;
      if (function) {
        NSError* pipelineError = nil;
        _layoutPipeline = [_device newComputePipelineStateWithFunction:function
                                                                  error:&pipelineError];
      }
      id<MTLFunction> cvFunction =
          library ? [library newFunctionWithName:@"layout_cv_yuv_copy"] : nil;
      if (cvFunction) {
        NSError* pipelineError = nil;
        _cvPixelBufferPipeline = [_device newComputePipelineStateWithFunction:cvFunction
                                                                        error:&pipelineError];
      }
    }
  }
  return self;
}

- (void)dealloc {
  if (_textureCache) {
    CFRelease(_textureCache);
    _textureCache = nullptr;
  }
}

- (BOOL)isAvailable {
  return _device != nil && _commandQueue != nil && _textureCache != nullptr;
}

- (int64_t)directYuvUploadCount {
  return _directYuvUploadCount.load(std::memory_order_relaxed);
}

- (int64_t)cvPixelBufferUploadCount {
  return _cvPixelBufferUploadCount.load(std::memory_order_relaxed);
}

- (int64_t)presentPackageUploadCount {
  return _presentPackageUploadCount.load(std::memory_order_relaxed);
}

- (int64_t)lastPresentPackageCopyUs {
  return _lastPresentPackageCopyUs.load(std::memory_order_relaxed);
}

- (int64_t)lastPresentPackageGpuWaitUs {
  return _lastPresentPackageGpuWaitUs.load(std::memory_order_relaxed);
}

- (int64_t)lastPresentPackageTotalUs {
  return _lastPresentPackageTotalUs.load(std::memory_order_relaxed);
}

- (int32_t)lastPresentPackageStorage {
  return _lastPresentPackageStorage.load(std::memory_order_relaxed);
}

- (int)validatePixelBufferStatus:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height {
  if (![self isAvailable] || !pixelBuffer || width <= 0 || height <= 0) {
    return [self isAvailable]
        ? VPMacOSMetalUploaderStatusInvalidArguments
        : VPMacOSMetalUploaderStatusUnavailable;
  }
  if (CVPixelBufferGetWidth(pixelBuffer) != static_cast<size_t>(width) ||
      CVPixelBufferGetHeight(pixelBuffer) != static_cast<size_t>(height)) {
    return VPMacOSMetalUploaderStatusSizeMismatch;
  }
  if (CVPixelBufferGetPixelFormatType(pixelBuffer) != kCVPixelFormatType_32BGRA) {
    return VPMacOSMetalUploaderStatusUnsupportedPixelFormat;
  }
  CVMetalTextureRef metalTextureRef = nullptr;
  const CVReturn status = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      _textureCache,
      pixelBuffer,
      nullptr,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &metalTextureRef);
  if (status != kCVReturnSuccess || !metalTextureRef) {
    return VPMacOSMetalUploaderStatusTextureWrapFailed;
  }
  id<MTLTexture> texture = CVMetalTextureGetTexture(metalTextureRef);
  const BOOL valid = texture != nil;
  CFRelease(metalTextureRef);
  return valid
      ? VPMacOSMetalUploaderStatusOk
      : VPMacOSMetalUploaderStatusTextureWrapFailed;
}

- (BOOL)validatePixelBuffer:(CVPixelBufferRef)pixelBuffer
                      width:(int32_t)width
                     height:(int32_t)height {
  return [self validatePixelBufferStatus:pixelBuffer width:width height:height] ==
      VPMacOSMetalUploaderStatusOk;
}

- (BOOL)ensureStagingBufferWithLength:(size_t)length {
  if (_stagingBuffer != nil && [_stagingBuffer length] >= length) {
    return YES;
  }
  _stagingBuffer = [_device newBufferWithLength:length
                                        options:MTLResourceStorageModeShared];
  return _stagingBuffer != nil;
}

- (BOOL)ensureLayoutParamsBuffer {
  if (_layoutParamsBuffer != nil && [_layoutParamsBuffer length] >= sizeof(MetalLayoutParams)) {
    return YES;
  }
  _layoutParamsBuffer = [_device newBufferWithLength:sizeof(MetalLayoutParams)
                                             options:MTLResourceStorageModeShared];
  return _layoutParamsBuffer != nil;
}

- (int)copyCurrentFrameFromPlayer:(VPMacOSNativePlayer*)player
                    toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height
                    waitTimeoutMs:(int32_t)waitTimeoutMs
                              out:(VPMacOSNativeFrameInfo*)out
                            error:(char*)error
                        errorSize:(size_t)errorSize {
  if (![self isAvailable]) {
    write_error(error, errorSize, "native Metal uploader is not available");
    return -1;
  }
  if (!player || !pixelBuffer || !out || width <= 0 || height <= 0) {
    write_error(error, errorSize, "invalid native Metal upload arguments");
    return -1;
  }
  const int validationStatus =
      [self validatePixelBufferStatus:pixelBuffer width:width height:height];
  if (validationStatus != VPMacOSMetalUploaderStatusOk) {
    write_error(error, errorSize, metal_uploader_status_message(validationStatus));
    return -1;
  }

  size_t rowBytes = 0;
  size_t uploadSize = 0;
  if (!checked_mul_size(static_cast<size_t>(width), 4u, &rowBytes) ||
      !checked_mul_size(rowBytes, static_cast<size_t>(height), &uploadSize)) {
    write_error(error, errorSize, "native Metal upload dimensions overflow");
    return -1;
  }
  if (rowBytes > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    write_error(error, errorSize, "native Metal upload row stride is too large");
    return -1;
  }
  if (![self ensureStagingBufferWithLength:uploadSize]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal staging buffer");
  }

  const int copyRet = VPMacOSNativePlayerCopyCurrentFrameBGRAInto(
      player,
      static_cast<uint8_t*>([_stagingBuffer contents]),
      uploadSize,
      width,
      height,
      static_cast<int32_t>(rowBytes),
      out,
      error,
      errorSize);
  if (copyRet != 0) {
    return copyRet;
  }

  CVMetalTextureRef metalTextureRef = nullptr;
  const CVReturn textureStatus = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      _textureCache,
      pixelBuffer,
      nullptr,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &metalTextureRef);
  if (textureStatus != kCVReturnSuccess || !metalTextureRef) {
    return metal_upload_failure(
        error, errorSize, "failed to wrap CVPixelBuffer as a Metal texture");
  }

  id<MTLTexture> destinationTexture = CVMetalTextureGetTexture(metalTextureRef);
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
  if (!destinationTexture || !commandBuffer || !blit) {
    CFRelease(metalTextureRef);
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal blit command");
  }

  [blit copyFromBuffer:_stagingBuffer
          sourceOffset:0
     sourceBytesPerRow:rowBytes
   sourceBytesPerImage:uploadSize
            sourceSize:MTLSizeMake(width, height, 1)
             toTexture:destinationTexture
      destinationSlice:0
      destinationLevel:0
     destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  const BOOL completed = [commandBuffer status] == MTLCommandBufferStatusCompleted;
  CFRelease(metalTextureRef);
  if (!completed) {
    return metal_upload_failure(
        error, errorSize, "native Metal blit did not complete");
  }

  write_error(error, errorSize, "");
  return 0;
}

- (int)copyPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           data:(const uint8_t*)data
                       dataSize:(size_t)dataSize
                  toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                          width:(int32_t)width
                         height:(int32_t)height
                            out:(VPMacOSNativeFrameInfo*)out
                          error:(char*)error
                      errorSize:(size_t)errorSize {
  if (![self isAvailable] || !_layoutPipeline) {
    write_error(error, errorSize, "native Metal layout uploader is not available");
    return -1;
  }
  if (!package || !data || dataSize == 0 || package->used_bytes == 0 ||
      package->used_bytes > dataSize) {
    write_error(error, errorSize, "invalid native Metal present package arguments");
    return -1;
  }
  if (![self ensureStagingBufferWithLength:package->used_bytes] ||
      ![self ensureLayoutParamsBuffer]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal layout buffers");
  }
  const auto totalStart = std::chrono::steady_clock::now();
  const auto copyStart = std::chrono::steady_clock::now();
  std::memcpy([_stagingBuffer contents], data, package->used_bytes);
  _lastPresentPackageCopyUs.store(elapsed_us_since(copyStart), std::memory_order_relaxed);
  const int uploadRet = [self uploadPreparedPresentFramePackage:package
                                                  toPixelBuffer:pixelBuffer
                                                          width:width
                                                         height:height
                                                            out:out
                                                          error:error
                                                      errorSize:errorSize];
  _lastPresentPackageTotalUs.store(elapsed_us_since(totalStart), std::memory_order_relaxed);
  return uploadRet;
}

- (int)uploadPreparedPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                   width:(int32_t)width
                                  height:(int32_t)height
                                     out:(VPMacOSNativeFrameInfo*)out
                                   error:(char*)error
                               errorSize:(size_t)errorSize {
  if (![self isAvailable] || !_layoutPipeline) {
    write_error(error, errorSize, "native Metal layout uploader is not available");
    return -1;
  }
  if (!package || !pixelBuffer || width <= 0 || height <= 0 ||
      package->storage == VPMacOSNativePresentPackageStorageUnavailable) {
    write_error(error, errorSize, "invalid native Metal present package arguments");
    return -1;
  }
  const int validationStatus =
      [self validatePixelBufferStatus:pixelBuffer width:width height:height];
  if (validationStatus != VPMacOSMetalUploaderStatusOk) {
    write_error(error, errorSize, metal_uploader_status_message(validationStatus));
    return -1;
  }

  const auto& decisionInfo = package->decision;
  if (package->storage == VPMacOSNativePresentPackageStorageYUV) {
    _directYuvUploadCount.fetch_add(1, std::memory_order_relaxed);
  }
  write_first_present_frame_info(decisionInfo, out);

  auto* metalParams = static_cast<MetalLayoutParams*>([_layoutParamsBuffer contents]);
  fill_metal_layout_params(*metalParams, decisionInfo, width, height);

  CVMetalTextureRef metalTextureRef = nullptr;
  const CVReturn textureStatus = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      _textureCache,
      pixelBuffer,
      nullptr,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &metalTextureRef);
  if (textureStatus != kCVReturnSuccess || !metalTextureRef) {
    return metal_upload_failure(
        error, errorSize, "failed to wrap CVPixelBuffer as a Metal texture");
  }

  id<MTLTexture> destinationTexture = CVMetalTextureGetTexture(metalTextureRef);
  const auto gpuStart = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
  if (!destinationTexture || !commandBuffer || !compute) {
    CFRelease(metalTextureRef);
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal layout compute command");
  }

  [compute setComputePipelineState:_layoutPipeline];
  [compute setBuffer:_stagingBuffer offset:0 atIndex:0];
  [compute setBuffer:_layoutParamsBuffer offset:0 atIndex:1];
  [compute setTexture:destinationTexture atIndex:0];

  const NSUInteger threadWidth = _layoutPipeline.threadExecutionWidth;
  const NSUInteger threadHeight =
      std::max<NSUInteger>(1, _layoutPipeline.maxTotalThreadsPerThreadgroup / threadWidth);
  const MTLSize threadsPerThreadgroup = MTLSizeMake(threadWidth, threadHeight, 1);
  const MTLSize threads = MTLSizeMake(width, height, 1);
  [compute dispatchThreads:threads threadsPerThreadgroup:threadsPerThreadgroup];
  [compute endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  const BOOL completed = [commandBuffer status] == MTLCommandBufferStatusCompleted;
  _lastPresentPackageGpuWaitUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
  CFRelease(metalTextureRef);
  if (!completed) {
    return metal_upload_failure(
        error, errorSize, "native Metal layout compute did not complete");
  }

  _presentPackageUploadCount.fetch_add(1, std::memory_order_relaxed);
  _lastPresentPackageStorage.store(package->storage, std::memory_order_relaxed);
  write_error(error, errorSize, "");
  return 0;
}

- (int)copyCVPixelBufferPresentFrame:(const VPMacOSNativeCVPixelBufferPresentFrame*)frame
                        toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                width:(int32_t)width
                               height:(int32_t)height
                                  out:(VPMacOSNativeFrameInfo*)out
                                error:(char*)error
                            errorSize:(size_t)errorSize {
  if (![self isAvailable] || !_cvPixelBufferPipeline) {
    write_error(error, errorSize, "native Metal CVPixelBuffer uploader is not available");
    return -1;
  }
  if (!frame || !frame->pixel_buffer || !pixelBuffer || !out ||
      width <= 0 || height <= 0 || frame->plane_count < 2 ||
      frame->coded_width <= 0 || frame->coded_height <= 0) {
    write_error(error, errorSize, "invalid native Metal CVPixelBuffer upload arguments");
    return -1;
  }
  const int validationStatus =
      [self validatePixelBufferStatus:pixelBuffer width:width height:height];
  if (validationStatus != VPMacOSMetalUploaderStatusOk) {
    write_error(error, errorSize, metal_uploader_status_message(validationStatus));
    return -1;
  }
  if (![self ensureLayoutParamsBuffer]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal layout buffers");
  }

  auto* metalParams = static_cast<MetalLayoutParams*>([_layoutParamsBuffer contents]);
  fill_metal_layout_params(*metalParams, frame->decision, width, height);
  write_first_present_frame_info(frame->decision, out);

  CVPixelBufferRef sourcePixelBuffer =
      static_cast<CVPixelBufferRef>(frame->pixel_buffer);
  const bool isP010 = frame->is_p010 != 0;
  const MTLPixelFormat yFormat = isP010 ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
  const MTLPixelFormat uvFormat = isP010 ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;
  CVMetalTextureRef sourceYRef = nullptr;
  CVMetalTextureRef sourceUVRef = nullptr;
  CVMetalTextureRef destinationRef = nullptr;
  const CVReturn yStatus = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      _textureCache,
      sourcePixelBuffer,
      nullptr,
      yFormat,
      CVPixelBufferGetWidthOfPlane(sourcePixelBuffer, 0),
      CVPixelBufferGetHeightOfPlane(sourcePixelBuffer, 0),
      0,
      &sourceYRef);
  const CVReturn uvStatus = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      _textureCache,
      sourcePixelBuffer,
      nullptr,
      uvFormat,
      CVPixelBufferGetWidthOfPlane(sourcePixelBuffer, 1),
      CVPixelBufferGetHeightOfPlane(sourcePixelBuffer, 1),
      1,
      &sourceUVRef);
  const CVReturn destinationStatus = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      _textureCache,
      pixelBuffer,
      nullptr,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &destinationRef);
  if (yStatus != kCVReturnSuccess || uvStatus != kCVReturnSuccess ||
      destinationStatus != kCVReturnSuccess || !sourceYRef || !sourceUVRef ||
      !destinationRef) {
    if (sourceYRef) CFRelease(sourceYRef);
    if (sourceUVRef) CFRelease(sourceUVRef);
    if (destinationRef) CFRelease(destinationRef);
    return metal_upload_failure(
        error, errorSize, "failed to wrap CVPixelBuffer planes as Metal textures");
  }

  id<MTLTexture> sourceYTexture = CVMetalTextureGetTexture(sourceYRef);
  id<MTLTexture> sourceUVTexture = CVMetalTextureGetTexture(sourceUVRef);
  id<MTLTexture> destinationTexture = CVMetalTextureGetTexture(destinationRef);
  const auto gpuStart = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
  if (!sourceYTexture || !sourceUVTexture || !destinationTexture ||
      !commandBuffer || !compute) {
    CFRelease(sourceYRef);
    CFRelease(sourceUVRef);
    CFRelease(destinationRef);
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal CVPixelBuffer compute command");
  }

  [compute setComputePipelineState:_cvPixelBufferPipeline];
  [compute setBuffer:_layoutParamsBuffer offset:0 atIndex:0];
  [compute setTexture:destinationTexture atIndex:0];
  [compute setTexture:sourceYTexture atIndex:1];
  [compute setTexture:sourceUVTexture atIndex:2];

  const NSUInteger threadWidth = _cvPixelBufferPipeline.threadExecutionWidth;
  const NSUInteger threadHeight =
      std::max<NSUInteger>(1, _cvPixelBufferPipeline.maxTotalThreadsPerThreadgroup / threadWidth);
  const MTLSize threadsPerThreadgroup = MTLSizeMake(threadWidth, threadHeight, 1);
  const MTLSize threads = MTLSizeMake(width, height, 1);
  [compute dispatchThreads:threads threadsPerThreadgroup:threadsPerThreadgroup];
  [compute endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  const BOOL completed = [commandBuffer status] == MTLCommandBufferStatusCompleted;
  _lastPresentPackageGpuWaitUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
  CFRelease(sourceYRef);
  CFRelease(sourceUVRef);
  CFRelease(destinationRef);
  if (!completed) {
    return metal_upload_failure(
        error, errorSize, "native Metal CVPixelBuffer compute did not complete");
  }

  _cvPixelBufferUploadCount.fetch_add(1, std::memory_order_relaxed);
  _lastPresentPackageCopyUs.store(0, std::memory_order_relaxed);
  _lastPresentPackageTotalUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
  _lastPresentPackageStorage.store(VPMacOSNativePresentPackageStorageCVPixelBuffer,
                                   std::memory_order_relaxed);
  write_error(error, errorSize, "");
  return 0;
}

- (int)copyCurrentFrameWithLayoutFromPlayer:(VPMacOSNativePlayer*)player
                              toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                      width:(int32_t)width
                                     height:(int32_t)height
                              maxTrackSlots:(int32_t)maxTrackSlots
                              waitTimeoutMs:(int32_t)waitTimeoutMs
                                        out:(VPMacOSNativeFrameInfo*)out
                                      error:(char*)error
                                  errorSize:(size_t)errorSize {
  if (![self isAvailable] || !_layoutPipeline) {
    write_error(error, errorSize, "native Metal layout uploader is not available");
    return -1;
  }
  if (!player || !pixelBuffer || !out || width <= 0 || height <= 0) {
    write_error(error, errorSize, "invalid native Metal layout upload arguments");
    return -1;
  }
  const int validationStatus =
      [self validatePixelBufferStatus:pixelBuffer width:width height:height];
  if (validationStatus != VPMacOSMetalUploaderStatusOk) {
    write_error(error, errorSize, metal_uploader_status_message(validationStatus));
    return -1;
  }

  const int32_t trackSlots =
      std::clamp(maxTrackSlots, static_cast<int32_t>(1), static_cast<int32_t>(VPMacOSNativeMaxTracks));
  const size_t stagingSize =
      VPMacOSNativePresentFramePackageMaxBytes(width, height, trackSlots);
  if (stagingSize == 0) {
    write_error(error, errorSize, "native Metal layout upload dimensions overflow");
    return -1;
  }
  if (![self ensureStagingBufferWithLength:stagingSize] ||
      ![self ensureLayoutParamsBuffer]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal layout buffers");
  }

  VPMacOSNativePresentFramePackageInfo package = {};
  const auto totalStart = std::chrono::steady_clock::now();
  const auto copyStart = std::chrono::steady_clock::now();
  const int copyRet = VPMacOSNativePlayerCopyPresentFramePackage(
      player,
      static_cast<uint8_t*>([_stagingBuffer contents]),
      stagingSize,
      width,
      height,
      trackSlots,
      &package,
      error,
      errorSize);
  _lastPresentPackageCopyUs.store(elapsed_us_since(copyStart), std::memory_order_relaxed);
  if (copyRet != 0) {
    if (error && std::strcmp(error, "not all present decision frames are ready") == 0) {
      return -1;
    }
    if (!error || error[0] == '\0') {
      write_error(error, errorSize, "failed to copy native present frames");
    }
    return -2;
  }
  const int uploadRet = [self uploadPreparedPresentFramePackage:&package
                                                  toPixelBuffer:pixelBuffer
                                                          width:width
                                                         height:height
                                                            out:out
                                                          error:error
                                                      errorSize:errorSize];
  _lastPresentPackageTotalUs.store(elapsed_us_since(totalStart), std::memory_order_relaxed);
  return uploadRet;
}

@end

struct VPMacOSMetalUploader {
  VPMacOSMetalUploaderImpl* impl;
};

VPMacOSMetalUploader* VPMacOSMetalUploaderCreate(void) {
  VPMacOSMetalUploaderImpl* impl = [[VPMacOSMetalUploaderImpl alloc] init];
  if (!impl) {
    return nullptr;
  }
  auto* uploader = new VPMacOSMetalUploader{impl};
  return uploader;
}

void VPMacOSMetalUploaderDestroy(VPMacOSMetalUploader* uploader) {
  delete uploader;
}

int VPMacOSMetalUploaderIsAvailable(VPMacOSMetalUploader* uploader) {
  return uploader && uploader->impl && [uploader->impl isAvailable] ? 1 : 0;
}

int64_t VPMacOSMetalUploaderDirectYUVUploadCount(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl directYuvUploadCount];
}

int64_t VPMacOSMetalUploaderCVPixelBufferUploadCount(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl cvPixelBufferUploadCount];
}

int64_t VPMacOSMetalUploaderPresentPackageUploadCount(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl presentPackageUploadCount];
}

int64_t VPMacOSMetalUploaderLastPresentPackageCopyUs(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl lastPresentPackageCopyUs];
}

int64_t VPMacOSMetalUploaderLastPresentPackageGpuWaitUs(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl lastPresentPackageGpuWaitUs];
}

int64_t VPMacOSMetalUploaderLastPresentPackageTotalUs(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl lastPresentPackageTotalUs];
}

int32_t VPMacOSMetalUploaderLastPresentPackageStorage(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return VPMacOSNativePresentPackageStorageUnavailable;
  }
  return [uploader->impl lastPresentPackageStorage];
}

int VPMacOSMetalUploaderValidatePixelBuffer(VPMacOSMetalUploader* uploader,
                                            void* pixel_buffer,
                                            int32_t width,
                                            int32_t height) {
  return VPMacOSMetalUploaderValidatePixelBufferChecked(
      uploader, pixel_buffer, width, height, nullptr, 0) ==
      VPMacOSMetalUploaderStatusOk ? 1 : 0;
}

const char* VPMacOSMetalUploaderStatusMessage(int status) {
  return metal_uploader_status_message(status);
}

int VPMacOSMetalUploaderValidatePixelBufferChecked(VPMacOSMetalUploader* uploader,
                                                   void* pixel_buffer,
                                                   int32_t width,
                                                   int32_t height,
                                                   char* error,
                                                   size_t error_size) {
  int status = VPMacOSMetalUploaderStatusUnavailable;
  if (uploader && uploader->impl) {
    status = [uploader->impl validatePixelBufferStatus:(CVPixelBufferRef)pixel_buffer
                                                width:width
                                               height:height];
  }
  write_error(error, error_size, metal_uploader_status_message(status));
  return status;
}

int VPMacOSMetalUploaderCopyCurrentFrame(VPMacOSMetalUploader* uploader,
                                         VPMacOSNativePlayer* player,
                                         void* pixel_buffer,
                                         int32_t width,
                                         int32_t height,
                                         int32_t wait_timeout_ms,
                                         VPMacOSNativeFrameInfo* out,
                                         char* error,
                                         size_t error_size) {
  if (!uploader || !uploader->impl) {
    write_error(error, error_size, "native Metal uploader is null");
    return -1;
  }
  return [uploader->impl copyCurrentFrameFromPlayer:player
                                     toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                             width:width
                                            height:height
                                     waitTimeoutMs:wait_timeout_ms
                                               out:out
                                             error:error
                                         errorSize:error_size];
}

int VPMacOSMetalUploaderCopyCurrentFrameWithLayout(
    VPMacOSMetalUploader* uploader,
    VPMacOSNativePlayer* player,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots,
    int32_t wait_timeout_ms,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  if (!uploader || !uploader->impl) {
    write_error(error, error_size, "native Metal uploader is null");
    return -1;
  }
  return [uploader->impl copyCurrentFrameWithLayoutFromPlayer:player
                                                toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                                        width:width
                                                       height:height
                                                maxTrackSlots:max_track_slots
                                                waitTimeoutMs:wait_timeout_ms
                                                          out:out
                                                        error:error
                                                    errorSize:error_size];
}

int VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
    VPMacOSMetalUploader* uploader,
    const uint8_t* data,
    size_t data_size,
    const VPMacOSNativePresentFramePackageInfo* package,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  if (!uploader || !uploader->impl) {
    write_error(error, error_size, "native Metal uploader is null");
    return -1;
  }
  return [uploader->impl copyPresentFramePackage:package
                                            data:data
                                        dataSize:data_size
                                   toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                           width:width
                                          height:height
                                             out:out
                                           error:error
                                       errorSize:error_size];
}

int VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayout(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeCVPixelBufferPresentFrame* frame,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  if (!uploader || !uploader->impl) {
    write_error(error, error_size, "native Metal uploader is null");
    return -1;
  }
  return [uploader->impl copyCVPixelBufferPresentFrame:frame
                                         toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                                 width:width
                                                height:height
                                                   out:out
                                                 error:error
                                             errorSize:error_size];
}
