#include "video_renderer/track_present_policy.h"

#include <algorithm>
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

std::optional<int64_t> first_present_decision_frame_pts_us(
    const PresentDecision& decision) {
    for (const auto& frame : decision.frames) {
        if (frame.has_value()) {
            return frame->pts_us;
        }
    }
    return std::nullopt;
}

void apply_present_carry_forward(
    const TrackPipelineManager& tracks,
    const PresentDecision& last_decision,
    PresentDecision& decision) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (decision.frames[i].has_value() ||
            !last_decision.frames[i].has_value() ||
            !tracks[i]) {
            continue;
        }

        const int64_t effective_pts =
            decision.current_pts_us - tracks[i]->offset_us;
        if (effective_pts >= 0) {
            decision.frames[i] = last_decision.frames[i];
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
        if (last_decision.frames[i].has_value()) {
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
