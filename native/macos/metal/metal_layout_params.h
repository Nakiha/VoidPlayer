#ifndef VOIDPLAYER_MACOS_METAL_LAYOUT_PARAMS_H_
#define VOIDPLAYER_MACOS_METAL_LAYOUT_PARAMS_H_

#include "macos/metal/metal_uploader_bridge.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace vp_macos {

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
  uint32_t overlay_present0;
  uint32_t overlay_present1;
  uint32_t overlay_present2;
  uint32_t overlay_present3;
  float background_color_r;
  float background_color_g;
  float background_color_b;
  float background_color_a;
  uint32_t output_edr;
};

struct MetalOverlayLayerParams {
  uint32_t width;
  uint32_t height;
  uint32_t track_slot;
  uint32_t reserved0;
};

struct MetalOverlayLinePassParams {
  uint32_t width;
  uint32_t height;
  uint32_t pass;
  uint32_t reserved0;
};

static_assert(VPMacOSNativeMaxTracks == 4,
              "Metal shader ABI flattens exactly four track slots");
static_assert(std::is_standard_layout<MetalLayoutParams>::value,
              "MetalLayoutParams must stay standard-layout for shader ABI offsets");
static_assert(sizeof(MetalLayoutParams) == 440,
              "MetalLayoutParams size must match the MSL LayoutParams mirror");
static_assert(offsetof(MetalLayoutParams, overlay_present0) == 404,
              "MetalLayoutParams overlay field offset must match MSL LayoutParams");
static_assert(offsetof(MetalLayoutParams, background_color_r) == 420,
              "MetalLayoutParams background color offset must match MSL LayoutParams");
static_assert(offsetof(MetalLayoutParams, output_edr) == 436,
              "MetalLayoutParams output field offset must match MSL LayoutParams");
static_assert(sizeof(MetalOverlayLayerParams) == 16,
              "MetalOverlayLayerParams must match the MSL constant layout");
static_assert(sizeof(MetalOverlayLinePassParams) == 16,
              "MetalOverlayLinePassParams must match the MSL constant layout");

void write_first_present_frame_info(const VPMacOSNativePresentDecisionInfo& decision_info,
                                    VPMacOSNativeFrameInfo* out);
void fill_metal_layout_params(MetalLayoutParams& metal_params,
                              const VPMacOSNativePresentDecisionInfo& decision_info,
                              int32_t width,
                              int32_t height);
void set_metal_overlay_present(MetalLayoutParams& metal_params,
                               const uint32_t overlay_present[VPMacOSNativeMaxTracks]);

}  // namespace vp_macos

#endif  // VOIDPLAYER_MACOS_METAL_LAYOUT_PARAMS_H_
