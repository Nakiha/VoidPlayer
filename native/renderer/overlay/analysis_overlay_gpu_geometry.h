#pragma once

#include "renderer/overlay/analysis_overlay_primitives.h"
#include "renderer/render/shader_constants.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vr {

struct AnalysisOverlayGpuVertex {
  float position_x = 0.0f;
  float position_y = 0.0f;
  float red = 0.0f;
  float green = 0.0f;
  float blue = 0.0f;
  float alpha = 0.0f;
  float contrast_axis = 0.0f;
  float contrast_center_px = 0.0f;
};

struct AnalysisOverlayGpuGeometry {
  std::vector<AnalysisOverlayGpuVertex> vertices;
  uint64_t fill_rect_count = 0;
  uint64_t line_rect_count = 0;
  size_t fill_vertex_count = 0;
  size_t contrast_vertex_count = 0;
  size_t motion_vertex_count = 0;
  bool empty() const { return vertices.empty(); }
};

// Converts codec-space primitives into API-neutral triangle-list NDC geometry
// through the same layout constants used by the native video shader.
AnalysisOverlayGpuGeometry build_analysis_overlay_gpu_geometry(
    const AnalysisOverlayPrimitivePackage& package,
    const ShaderConstants& constants,
    int target_width,
    int target_height);

}  // namespace vr
