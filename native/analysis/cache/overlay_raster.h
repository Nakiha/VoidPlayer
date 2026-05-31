#pragma once

#include "analysis/cache/overlay_chunk.h"

#include <cstdint>
#include <vector>

namespace vr::analysis {

struct OverlayColor {
    uint8_t b = 0;
    uint8_t g = 0;
    uint8_t r = 0;
    uint8_t a = 0;
};

enum class OverlayHeatmapMode {
    Qp,
    BitCost,
};

inline constexpr uint8_t kOverlayQpHeatmapMax = 63;
inline constexpr uint64_t kOverlayBitDensityHeatmapMax = 4096;

struct OverlayRasterStats {
    uint64_t cu_count = 0;
    uint64_t filled_pixels = 0;
};

void blend_overlay_pixel(std::vector<uint8_t>& pixels,
                         int width,
                         int height,
                         int x,
                         int y,
                         OverlayColor color);

void fill_overlay_rect(std::vector<uint8_t>& pixels,
                       int width,
                       int height,
                       int x0,
                       int y0,
                       int x1,
                       int y1,
                       OverlayColor color,
                       OverlayRasterStats* stats = nullptr);

void set_overlay_mask_pixel(std::vector<uint8_t>& pixels,
                            int width,
                            int height,
                            int x,
                            int y);

void stroke_overlay_rect_mask(std::vector<uint8_t>& pixels,
                              int width,
                              int height,
                              int x0,
                              int y0,
                              int x1,
                              int y1);

void set_overlay_mask_pixel8(std::vector<uint8_t>& pixels,
                             int width,
                             int height,
                             int x,
                             int y);

void stroke_overlay_rect_mask8(std::vector<uint8_t>& pixels,
                               int width,
                               int height,
                               int x0,
                               int y0,
                               int x1,
                               int y1);

void draw_overlay_line(std::vector<uint8_t>& pixels,
                       int width,
                       int height,
                       int x0,
                       int y0,
                       int x1,
                       int y1,
                       OverlayColor color);

bool overlay_frame_covers_surface(const VachunkOverlayFrameData& frame,
                                  uint32_t video_width,
                                  uint32_t video_height,
                                  int surface_width,
                                  int surface_height);

OverlayColor heatmap_ramp_color(float value, uint8_t alpha);
uint8_t qp_heatmap_clamped_value(uint8_t qp);
uint64_t cu_bit_density_normalized_64x64(const VachunkCuCommon& cu);
OverlayColor qp_color(uint8_t qp, uint8_t alpha);
OverlayColor cu_bit_density_color(const VachunkCuCommon& cu, uint8_t alpha);
OverlayColor pred_color(uint8_t pred_mode, const VachunkCuInter& inter, uint8_t alpha);

bool raster_overlay_heatmap(const VachunkOverlayFrameData& frame,
                            uint32_t video_width,
                            uint32_t video_height,
                            int surface_width,
                            int surface_height,
                            OverlayHeatmapMode mode,
                            uint8_t alpha,
                            std::vector<uint8_t>& pixels,
                            OverlayRasterStats* stats = nullptr);

} // namespace vr::analysis
