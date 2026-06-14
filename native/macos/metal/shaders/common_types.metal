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
  float background_color_r;
  float background_color_g;
  float background_color_b;
  float background_color_a;
  uint output_edr;
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

float4 viewport_background_color(constant LayoutParams& params) {
  return saturate(float4(params.background_color_r,
                         params.background_color_g,
                         params.background_color_b,
                         params.background_color_a));
}
