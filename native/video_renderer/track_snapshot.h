#pragma once

#include "video_renderer/track_info.h"
#include "video_renderer/track_pipeline.h"

#include <vector>

namespace vr {

std::vector<TrackInfo> snapshot_track_infos(const TrackPipelineManager& tracks);

} // namespace vr
