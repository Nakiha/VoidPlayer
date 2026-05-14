#pragma once

#include "video_renderer/track_pipeline.h"

#include <functional>

namespace vr {

bool has_buffering_track(const TrackPipelineManager& tracks);

void apply_track_video_decode_pause_state(
    TrackPipelineManager& tracks,
    bool paused,
    std::function<void(size_t slot, TrackPipeline& track, bool paused)>
        set_decode_paused);

bool retreat_tracks_if_all_can_retreat(TrackPipelineManager& tracks);

bool build_step_forward_decision(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us,
    int64_t frame_duration_us,
    const PresentDecision& last_decision,
    PresentDecision& decision);

void discard_step_forward_consumed_frames(
    TrackPipelineManager& tracks,
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision);

} // namespace vr
