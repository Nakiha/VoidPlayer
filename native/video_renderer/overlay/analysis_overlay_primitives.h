#pragma once

#include "analysis/cache/overlay_raster.h"
#include "video_renderer/render/renderer_draw_snapshot.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace vr {

struct AnalysisOverlayRectPrimitive {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    analysis::OverlayColor color{};
};

struct AnalysisOverlayLinePrimitive {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;
    analysis::OverlayColor color{};
};

struct AnalysisOverlayTrackPrimitives {
    int slot = -1;
    int track_file_id = -1;
    int frame_index = -1;
    int video_width = 0;
    int video_height = 0;
    int mode = -1;
    int opacity_permille = -1;
    bool show_grid = false;
    bool show_qp = false;
    bool show_pred = false;
    bool show_lines = false;
    bool show_bit_cost = false;
    uint8_t line_alpha = 0;
    std::vector<AnalysisOverlayRectPrimitive> fill_rects;
    std::vector<AnalysisOverlayRectPrimitive> outline_rects;
    std::vector<AnalysisOverlayLinePrimitive> motion_lines;
};

struct AnalysisOverlayPrimitivePackage {
    uint64_t cache_generation = 0;
    std::vector<AnalysisOverlayTrackPrimitives> tracks;

    bool empty() const { return tracks.empty(); }
};

AnalysisOverlayPrimitivePackage build_analysis_overlay_primitives(
    const RendererDrawSnapshot& snapshot);

std::shared_ptr<const AnalysisOverlayPrimitivePackage>
build_analysis_overlay_primitive_package(const RendererDrawSnapshot& snapshot);

} // namespace vr
