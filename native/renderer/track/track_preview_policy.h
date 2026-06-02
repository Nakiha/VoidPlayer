#pragma once

#include "renderer/track/track_pipeline.h"

namespace vr {

struct PausedPreviewSnapshot {
    PresentDecision decision;
    bool ready_to_present = false;
};

struct AvailablePausedFrameSnapshot {
    PresentDecision decision;
    bool has_frame = false;
};

PausedPreviewSnapshot build_paused_preview_snapshot(
    const TrackPipelineManager& tracks);

AvailablePausedFrameSnapshot build_available_paused_frame_snapshot(
    const TrackPipelineManager& tracks);

} // namespace vr
