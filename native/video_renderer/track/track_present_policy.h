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

struct PlaybackEofSettlementInput {
    bool playing = false;
    int64_t current_pts_us = 0;
    int64_t last_presented_end_us = 0;
    int64_t reported_duration_us = 0;
    int64_t frame_duration_us = 0;
};

struct PlaybackEofSettlementDecision {
    bool should_settle = false;
    bool should_clamp_clock = false;
    bool used_reported_duration = false;
    int64_t settle_pts_us = 0;
    int64_t tolerance_us = 0;
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

bool present_decision_slot_matches_track(
    const PresentDecision& decision,
    const TrackPipelineManager& tracks,
    size_t slot);

void set_present_decision_track_identity(
    PresentDecision& decision,
    size_t slot,
    const TrackPipeline& track);

void clear_present_decision_slot(PresentDecision& decision, size_t slot);

void filter_present_decision_against_tracks(
    PresentDecision& decision,
    const TrackPipelineManager& tracks);

std::optional<int64_t> first_present_decision_frame_pts_us(
    const PresentDecision& decision);

PresentDecision peek_present_decision_for_track_file_id(
    const TrackPipelineManager& tracks,
    int file_id);

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

PlaybackEofSettlementDecision choose_playback_eof_settlement(
    const PlaybackEofSettlementInput& input);

std::optional<int64_t> compute_next_frame_event_pts_us(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us);

} // namespace vr
