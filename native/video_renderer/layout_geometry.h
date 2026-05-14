#pragma once

#include "video_renderer/layout_state.h"
#include "video_renderer/shader_constants.h"
#include "video_renderer/sync/render_sink.h"

#include <array>
#include <utility>

namespace vr {

struct LayoutTrackGeometry {
    bool active = false;
    int width = 0;
    int height = 0;
    float aspect = 1.0f;
};

using LayoutTrackGeometryList = std::array<LayoutTrackGeometry, kMaxTracks>;

std::pair<float, float> display_pixel_size_for_layout(
    int width,
    int height,
    const LayoutState& layout,
    const LayoutTrackGeometryList& tracks);

void populate_layout_shader_constants(ShaderConstants& constants,
                                      const LayoutState& layout,
                                      const LayoutTrackGeometryList& tracks,
                                      int canvas_width,
                                      int canvas_height);

} // namespace vr
