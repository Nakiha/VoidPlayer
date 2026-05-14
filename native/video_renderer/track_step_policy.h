#pragma once

#include "video_renderer/track_pipeline.h"

#include <functional>

namespace vr {

bool has_buffering_track(const TrackPipelineManager& tracks);

void apply_track_video_decode_pause_state(
    TrackPipelineManager& tracks,
    bool paused,
    std::function<void(size_t slot, TrackPipeline& track, bool paused)>
        set_decode_paused);

bool retreat_tracks_if_all_can_retreat(TrackPipelineManager& tracks);

} // namespace vr
