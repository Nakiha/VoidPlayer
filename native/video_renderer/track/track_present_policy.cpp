#include "video_renderer/track/track_present_policy.h"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace vr {

bool present_decision_has_frame(const PresentDecision& decision) {
    for (const auto& frame : decision.frames) {
        if (frame.has_value()) {
            return true;
        }
    }
    return false;
}

bool present_decision_slot_matches_track(
    const PresentDecision& decision,
    const TrackPipelineManager& tracks,
    size_t slot) {
    if (slot >= kMaxTracks || !decision.frames[slot].has_value() || !tracks[slot]) {
        return false;
    }
    if (tracks[slot]->generation == 0 &&
        decision.file_ids[slot] == -1 &&
        decision.track_generations[slot] == 0) {
        return true;
    }
    return decision.file_ids[slot] == tracks[slot]->file_id &&
           decision.track_generations[slot] == tracks[slot]->generation;
}

void set_present_decision_track_identity(
    PresentDecision& decision,
    size_t slot,
    const TrackPipeline& track) {
    if (slot >= kMaxTracks) {
        return;
    }
    decision.file_ids[slot] = track.file_id;
    decision.track_generations[slot] = track.generation;
}

void clear_present_decision_slot(PresentDecision& decision, size_t slot) {
    if (slot >= kMaxTracks) {
        return;
    }
    decision.frames[slot] = std::nullopt;
    decision.file_ids[slot] = -1;
    decision.track_generations[slot] = 0;
}

void filter_present_decision_against_tracks(
    PresentDecision& decision,
    const TrackPipelineManager& tracks) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!decision.frames[i].has_value()) {
            if (!tracks[i]) {
                clear_present_decision_slot(decision, i);
            }
            continue;
        }
        if (!present_decision_slot_matches_track(decision, tracks, i)) {
            clear_present_decision_slot(decision, i);
        }
    }
    decision.should_present = present_decision_has_frame(decision);
}

std::optional<int64_t> first_present_decision_frame_pts_us(
    const PresentDecision& decision) {
    for (const auto& frame : decision.frames) {
        if (frame.has_value()) {
            return frame->pts_us;
        }
    }
    return std::nullopt;
}

std::vector<SeekPreviewPresentedTrackEvent>
collect_seek_preview_presented_track_events(
    const TrackPipelineManager& tracks,
    const PresentDecision& decision,
    int64_t request_id,
    int64_t target_pts_us) {
    std::vector<SeekPreviewPresentedTrackEvent> events;

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!present_decision_slot_matches_track(decision, tracks, i)) {
            continue;
        }
        const int track_file_id = tracks[i]->file_id;
        if (track_file_id < 0) {
            continue;
        }

        SeekPreviewPresentedTrackEvent event;
        event.slot = i;
        event.file_id = track_file_id;
        event.request_id = request_id;
        event.pts_us = decision.frames[i]->pts_us;
        event.dts_us = decision.frames[i]->dts_us;
        event.target_pts_us = target_pts_us;
        events.push_back(event);
    }

    return events;
}

void apply_present_carry_forward(
    const TrackPipelineManager& tracks,
    const PresentDecision& last_decision,
    PresentDecision& decision) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (decision.frames[i].has_value() ||
            !last_decision.frames[i].has_value() ||
            !tracks[i] ||
            !present_decision_slot_matches_track(last_decision, tracks, i)) {
            continue;
        }

        const int64_t effective_pts =
            decision.current_pts_us - tracks[i]->offset_us;
        if (effective_pts >= 0) {
            decision.frames[i] = last_decision.frames[i];
            set_present_decision_track_identity(decision, i, *tracks[i]);
        }
    }
}

EmptyBufferEofClamp compute_empty_buffer_eof_clamp(
    const TrackPipelineManager& tracks,
    const PresentDecision& last_decision) {
    EmptyBufferEofClamp clamp;

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        if (tracks[i]->track_buffer &&
            tracks[i]->track_buffer->peek(0).has_value()) {
            clamp.all_active_buffers_empty = false;
            break;
        }
        if (present_decision_slot_matches_track(last_decision, tracks, i)) {
            clamp.max_end_pts_us = std::max(
                clamp.max_end_pts_us,
                last_decision.frames[i]->pts_us +
                    last_decision.frames[i]->duration_us +
                    tracks[i]->offset_us);
        }
    }

    return clamp;
}

std::optional<int64_t> compute_next_frame_event_pts_us(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us) {
    std::optional<int64_t> next_event_pts;

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i] || !tracks[i]->track_buffer) {
            continue;
        }
        const auto frame = tracks[i]->track_buffer->peek(0);
        if (!frame.has_value()) {
            continue;
        }

        const int64_t event_pts =
            frame->pts_us > current_pts_us
                ? frame->pts_us
                : frame->pts_us + frame->duration_us;
        if (!next_event_pts.has_value() || event_pts < *next_event_pts) {
            next_event_pts = event_pts;
        }
    }

    return next_event_pts;
}

} // namespace vr
