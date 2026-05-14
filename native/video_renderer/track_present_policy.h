#pragma once

#include "video_renderer/track_pipeline.h"

namespace vr {

void apply_present_carry_forward(
    const TrackPipelineManager& tracks,
    const PresentDecision& last_decision,
    PresentDecision& decision);

} // namespace vr
