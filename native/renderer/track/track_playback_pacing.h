#pragma once

#include "renderer/playback/playback_pacing_controller.h"
#include "renderer/track/track_pipeline.h"

#include <cstdint>

namespace vr {

// Builds platform-neutral pacing facts from track queues. The snapshot does
// not retain frames and does not mutate any queue cursor.
PlaybackPacingSnapshot snapshot_track_playback_pacing(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us);

} // namespace vr
