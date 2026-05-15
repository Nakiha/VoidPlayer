#pragma once

#include "video_renderer/layout_geometry.h"
#include "video_renderer/sync/render_sink.h"

#include <array>
#include <cstdint>

namespace vr {

struct RendererDrawTrackSnapshot {
    bool active = false;
    int file_id = -1;
    int64_t offset_us = 0;
    int video_width = 0;
    int video_height = 0;
    float video_aspect = 1.0f;
};

using RendererDrawTrackSnapshotList =
    std::array<RendererDrawTrackSnapshot, kMaxTracks>;

struct RendererDrawSnapshot {
    PresentDecision decision;
    LayoutState layout;
    LayoutTrackGeometryList track_geometry{};
    RendererDrawTrackSnapshotList tracks{};
    float background_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int target_width = 0;
    int target_height = 0;
};

} // namespace vr
