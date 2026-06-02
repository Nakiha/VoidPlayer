#include "renderer/track/track_preview_policy.h"
#include "renderer/track/track_present_policy.h"

#include <optional>

namespace vr {

PausedPreviewSnapshot build_paused_preview_snapshot(
    const TrackPipelineManager& tracks) {
    PausedPreviewSnapshot snapshot;
    snapshot.decision.current_pts_us = 0;
    snapshot.decision.should_present = false;

    bool all_active_ready = true;
    bool all_active_have_frames = true;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            snapshot.decision.frames[i] = std::nullopt;
            continue;
        }
        if (!tracks[i]->track_buffer) {
            all_active_ready = false;
            all_active_have_frames = false;
            continue;
        }

        const auto state = tracks[i]->track_buffer->state();
        if (state != TrackState::Ready) {
            all_active_ready = false;
        }

        auto frame = tracks[i]->track_buffer->peek(0);
        if (frame.has_value()) {
            snapshot.decision.frames[i] = frame;
            set_present_decision_track_identity(snapshot.decision, i, *tracks[i]);
        } else {
            all_active_have_frames = false;
        }
    }

    snapshot.ready_to_present =
        all_active_ready &&
        all_active_have_frames &&
        present_decision_has_frame(snapshot.decision);
    snapshot.decision.should_present = snapshot.ready_to_present;
    return snapshot;
}

AvailablePausedFrameSnapshot build_available_paused_frame_snapshot(
    const TrackPipelineManager& tracks) {
    AvailablePausedFrameSnapshot snapshot;
    snapshot.decision.current_pts_us = 0;
    snapshot.decision.should_present = false;

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            snapshot.decision.frames[i] = std::nullopt;
            continue;
        }
        if (!tracks[i]->track_buffer) {
            continue;
        }
        auto frame = tracks[i]->track_buffer->peek(0);
        if (frame.has_value()) {
            snapshot.decision.frames[i] = frame;
            set_present_decision_track_identity(snapshot.decision, i, *tracks[i]);
            snapshot.has_frame = true;
        }
    }

    return snapshot;
}

} // namespace vr
