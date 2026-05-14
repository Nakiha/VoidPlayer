#include "video_renderer/track_step_policy.h"

namespace vr {

bool has_buffering_track(const TrackPipelineManager& tracks) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i] || !tracks[i]->track_buffer) {
            continue;
        }
        if (tracks[i]->track_buffer->state() == TrackState::Buffering) {
            return true;
        }
    }
    return false;
}

void apply_track_video_decode_pause_state(
    TrackPipelineManager& tracks,
    bool paused,
    std::function<void(size_t slot, TrackPipeline& track, bool paused)>
        set_decode_paused) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        if (set_decode_paused) {
            set_decode_paused(i, *tracks[i], paused);
        }
    }
}

bool retreat_tracks_if_all_can_retreat(TrackPipelineManager& tracks) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        if (!tracks[i]->track_buffer || !tracks[i]->track_buffer->can_retreat()) {
            return false;
        }
    }

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i] || !tracks[i]->track_buffer) {
            continue;
        }
        tracks[i]->track_buffer->retreat();
    }
    return true;
}

} // namespace vr
