#pragma once

#include "video_renderer/track/track_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vr {

struct EmptyBufferEofClamp {
    bool all_active_buffers_empty = true;
    int64_t max_end_pts_us = 0;
};

struct SeekPreviewPresentedTrackEvent {
    size_t slot = 0;
    int file_id = -1;
    int64_t request_id = -1;
    int64_t pts_us = -1;
    int64_t dts_us = kNoTimestampUs;
    int64_t target_pts_us = -1;
};

bool present_decision_has_frame(const PresentDecision& decision);

std::optional<int64_t> first_present_decision_frame_pts_us(
    const PresentDecision& decision);

std::vector<SeekPreviewPresentedTrackEvent>
collect_seek_preview_presented_track_events(
    const TrackPipelineManager& tracks,
    const PresentDecision& decision,
    int64_t request_id,
    int64_t target_pts_us);

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
