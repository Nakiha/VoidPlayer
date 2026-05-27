#pragma once

#include "analysis/cache/overlay_chunk.h"
#include "analysis/cache/overlay_raster.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vr::analysis {

enum class OverlayCuLabelMode {
    Qp,
    BitCost,
    Prediction,
};

struct OverlayGlyphQuad {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    int atlas_x0 = 0;
    int atlas_y0 = 0;
    int atlas_x1 = 0;
    int atlas_y1 = 0;
    OverlayColor color{};
};

struct OverlayTextLayout {
    int surface_width = 0;
    int surface_height = 0;
    float rect_x0 = 0.0f;
    float rect_y0 = 0.0f;
    float rect_x1 = 0.0f;
    float rect_y1 = 0.0f;
    float pixel_scale_x = 1.0f;
    float pixel_scale_y = 1.0f;
    int target_cell_pixels = 2;
    int padding_pixels = 4;
};

std::string overlay_cu_label_text(const VachunkCuRecord& cu,
                                  OverlayCuLabelMode mode);

bool append_ascii_overlay_glyph_quads(std::vector<OverlayGlyphQuad>& out,
                                      std::string_view text,
                                      const OverlayTextLayout& layout,
                                      OverlayColor foreground,
                                      OverlayColor shadow);

bool build_ascii_overlay_glyph_atlas(std::vector<uint8_t>& alpha,
                                     int& width,
                                     int& height);

} // namespace vr::analysis
