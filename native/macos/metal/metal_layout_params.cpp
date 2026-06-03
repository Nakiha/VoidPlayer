#include "macos/metal/metal_layout_params.h"

#include <algorithm>

namespace vp_macos {

void write_first_present_frame_info(const VPMacOSNativePresentDecisionInfo& decisionInfo,
                                    VPMacOSNativeFrameInfo* out) {
  if (!out) {
    return;
  }
  VPMacOSNativeFrameInfoInit(out);
  for (int slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    if (decisionInfo.frames[slot].present) {
      out->width = decisionInfo.frames[slot].width;
      out->height = decisionInfo.frames[slot].height;
      out->pts_us = decisionInfo.frames[slot].pts_us;
      out->dts_us = decisionInfo.frames[slot].dts_us;
      out->duration_us = decisionInfo.frames[slot].duration_us;
      out->analysis_frame_index = decisionInfo.frames[slot].analysis_frame_index;
      out->frame_identity_mode = decisionInfo.frames[slot].frame_identity_mode;
      out->source_packet_index = decisionInfo.frames[slot].source_packet_index;
      out->source_packet_size = decisionInfo.frames[slot].source_packet_size;
      out->source_packet_pos = decisionInfo.frames[slot].source_packet_pos;
      out->source_packet_pts = decisionInfo.frames[slot].source_packet_pts;
      out->source_packet_dts = decisionInfo.frames[slot].source_packet_dts;
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
  metalParams.overlay_present0 = 0;
  metalParams.overlay_present1 = 0;
  metalParams.overlay_present2 = 0;
  metalParams.overlay_present3 = 0;
  metalParams.background_color_r = decisionInfo.background_color[0];
  metalParams.background_color_g = decisionInfo.background_color[1];
  metalParams.background_color_b = decisionInfo.background_color[2];
  metalParams.background_color_a = decisionInfo.background_color[3];
}

void set_metal_overlay_present(MetalLayoutParams& metalParams,
                               const uint32_t overlayPresent[VPMacOSNativeMaxTracks]) {
  metalParams.overlay_present0 = overlayPresent ? overlayPresent[0] : 0;
  metalParams.overlay_present1 = overlayPresent ? overlayPresent[1] : 0;
  metalParams.overlay_present2 = overlayPresent ? overlayPresent[2] : 0;
  metalParams.overlay_present3 = overlayPresent ? overlayPresent[3] : 0;
}

}  // namespace vp_macos
