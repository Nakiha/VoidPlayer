#include "video_renderer/track_preroll_policy.h"

namespace vr {

bool has_preroll_blocking_track(const TrackPipelineManager& tracks) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        if (!tracks[i]->track_buffer) {
            return true;
        }
        const auto state = tracks[i]->track_buffer->state();
        if (state == TrackState::Buffering ||
            state == TrackState::Empty ||
            state == TrackState::Flushing) {
            return true;
        }
    }
    return false;
}

} // namespace vr
