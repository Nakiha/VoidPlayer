#pragma once

#include "renderer/render/presentation_backend_types.h"
#include "renderer/track/track_info.h"

#include <array>
#include <string>
#include <vector>

namespace vr {

struct PresentationSourceCacheRequest {
    std::array<bool, 4> requested_slots{};
    std::array<int, 4> source_order = {0, 1, 2, 3};
};

struct PresentationSourceCachePlan {
    bool complete = false;
    std::vector<PresentationSourceCacheTrackDescriptor> tracks;
    std::string signature;
    std::string error = "none";
};

PresentationSourceCachePlan build_presentation_source_cache_plan(
    const PresentationSourceCacheRequest& request,
    const std::vector<TrackInfo>& tracks);

} // namespace vr
