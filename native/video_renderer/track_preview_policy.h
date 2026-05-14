#pragma once

#include "video_renderer/track_pipeline.h"

namespace vr {

struct PausedPreviewSnapshot {
    PresentDecision decision;
    bool ready_to_present = false;
};

PausedPreviewSnapshot build_paused_preview_snapshot(
    const TrackPipelineManager& tracks);

} // namespace vr
