#include "video_renderer/track_present_policy.h"

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

} // namespace vr
