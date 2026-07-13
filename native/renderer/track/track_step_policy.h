#pragma once

#include "renderer/track/track_pipeline.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace vr {

bool has_buffering_track(const TrackPipelineManager& tracks);

void apply_track_video_decode_pause_state(
    TrackPipelineManager& tracks,
    bool paused,
    std::function<void(size_t slot, TrackPipeline& track, bool paused)>
        set_decode_paused);

int64_t compute_min_current_frame_duration_us(
    const TrackPipelineManager& tracks);

struct StepDecisionBuildResult {
    bool has_decision = false;
    bool needs_lookahead = false;
};

StepDecisionBuildResult build_step_forward_decision_result(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us,
    int64_t frame_duration_us,
    const PresentDecision& last_decision,
    PresentDecision& decision);

bool build_step_forward_decision(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us,
    int64_t frame_duration_us,
    const PresentDecision& last_decision,
    PresentDecision& decision);

struct StepDecisionApplication {
    int reference_slot = -1;
    bool has_clock_target = false;
    int64_t clock_target_us = 0;
    int64_t presented_pts_us = 0;
};

StepDecisionApplication apply_step_forward_decision(
    TrackPipelineManager& tracks,
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision);

bool build_step_backward_decision(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us,
    const PresentDecision& last_decision,
    PresentDecision& decision);

StepDecisionApplication apply_step_backward_decision(
    TrackPipelineManager& tracks,
    const PresentDecision& decision);

struct StepForwardExactSeekTarget {
    int reference_slot = -1;
    int64_t base_pts_us = 0;
    int64_t clock_pts_us = 0;
    int64_t frame_duration_us = 0;
    int64_t target_pts_us = 0;
    int64_t decode_target_pts_us = 0;
    int64_t visible_pts_us = 0;
    bool has_visible_pts = false;
    bool clamped_to_duration = false;
};

StepForwardExactSeekTarget choose_step_forward_exact_seek_target(
    const TrackPipelineManager& tracks,
    int64_t clock_pts_us,
    int64_t cached_duration_us,
    const PresentDecision& last_decision,
    std::optional<int64_t> logical_step_anchor_us = std::nullopt);

struct StepBackwardExactSeekTarget {
    int reference_slot = -1;
    int64_t base_pts_us = 0;
    int64_t clock_pts_us = 0;
    int64_t frame_duration_us = 0;
    int64_t target_pts_us = 0;
    bool clamped_to_zero = false;
};

StepBackwardExactSeekTarget choose_step_backward_exact_seek_target(
    const TrackPipelineManager& tracks,
    int64_t clock_pts_us,
    const PresentDecision& last_decision);

void discard_step_forward_consumed_frames(
    TrackPipelineManager& tracks,
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision);

} // namespace vr
