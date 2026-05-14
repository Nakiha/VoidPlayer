#pragma once

#include "video_renderer/track_pipeline.h"

#include <cstdint>

namespace vr {

struct EmptyBufferEofClamp {
    bool all_active_buffers_empty = true;
    int64_t max_end_pts_us = 0;
};

void apply_present_carry_forward(
    const TrackPipelineManager& tracks,
    const PresentDecision& last_decision,
    PresentDecision& decision);

EmptyBufferEofClamp compute_empty_buffer_eof_clamp(
    const TrackPipelineManager& tracks,
    const PresentDecision& last_decision);

} // namespace vr
