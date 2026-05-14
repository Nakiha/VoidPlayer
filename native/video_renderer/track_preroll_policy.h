#pragma once

#include "video_renderer/track_pipeline.h"

namespace vr {

bool has_preroll_blocking_track(const TrackPipelineManager& tracks);

} // namespace vr
