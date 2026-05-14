#pragma once

#include "video_renderer/track_pipeline.h"

#include <cstdint>
#include <optional>

namespace vr {

struct EmptyBufferEofClamp {
    bool all_active_buffers_empty = true;
    int64_t max_end_pts_us = 0;
};

bool present_decision_has_frame(const PresentDecision& decision);

std::optional<int64_t> first_present_decision_frame_pts_us(
    const PresentDecision& decision);

void apply_present_carry_forward(
    const TrackPipelineManager& tracks,
    const PresentDecision& last_decision,
    PresentDecision& decision);

EmptyBufferEofClamp compute_empty_buffer_eof_clamp(
    const TrackPipelineManager& tracks,
    const PresentDecision& last_decision);

std::optional<int64_t> compute_next_frame_event_pts_us(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us);

} // namespace vr
