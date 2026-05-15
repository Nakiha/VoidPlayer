#pragma once

#include "video_renderer/layout_state.h"
#include "video_renderer/shader_constants.h"
#include "video_renderer/sync/render_sink.h"
#include "video_renderer/track_pipeline.h"

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace vr {

struct LayoutTrackGeometry {
    bool active = false;
    int width = 0;
    int height = 0;
    float aspect = 1.0f;
};

struct LayoutTrackGeometryUpdate {
    size_t slot = 0;
    int old_width = 0;
    int old_height = 0;
    float old_aspect = 1.0f;
    int new_width = 0;
    int new_height = 0;
    float new_aspect = 1.0f;
};

using LayoutTrackGeometryList = std::array<LayoutTrackGeometry, kMaxTracks>;

struct LayoutResizeViewOffsetAdjustment {
    bool adjusted_x = false;
    bool adjusted_y = false;
    float old_offset_x = 0.0f;
    float old_offset_y = 0.0f;
    float new_offset_x = 0.0f;
    float new_offset_y = 0.0f;
};

LayoutTrackGeometryList snapshot_layout_track_geometry(
    const TrackPipelineManager& tracks);

std::vector<LayoutTrackGeometryUpdate> update_layout_track_geometry_from_decision(
    TrackPipelineManager& tracks,
    const PresentDecision& decision);

std::pair<float, float> display_pixel_size_for_layout(
    int width,
    int height,
    const LayoutState& layout,
    const LayoutTrackGeometryList& tracks);

LayoutResizeViewOffsetAdjustment adjust_layout_view_offset_for_resize(
    LayoutState& layout,
    int old_width,
    int old_height,
    int new_width,
    int new_height,
    const LayoutTrackGeometryList& tracks);

void populate_layout_shader_constants(ShaderConstants& constants,
                                      const LayoutState& layout,
                                      const LayoutTrackGeometryList& tracks,
                                      int canvas_width,
                                      int canvas_height);

} // namespace vr
