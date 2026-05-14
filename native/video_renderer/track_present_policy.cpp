#include "video_renderer/track_present_policy.h"

#include <algorithm>

namespace vr {

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

} // namespace vr
