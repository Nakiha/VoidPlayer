#pragma once

#include "media/demux_thread.h"
#include "renderer/renderer_limits.h"

#include <cstddef>

namespace vr {

struct TrackBufferBudgetDecision {
    size_t forward_depth = 0;
    size_t backward_depth = 0;
    bool high_resolution = false;

    size_t max_cached_frames() const {
        return forward_depth + backward_depth;
    }
};

bool is_high_resolution_track(
    const DemuxStats& stats,
    const NativeResourceBudget& budget = default_native_resource_budget());

TrackBufferBudgetDecision choose_track_buffer_budget(
    const DemuxStats& stats,
    bool hw_decode,
    const NativeResourceBudget& budget = default_native_resource_budget());

} // namespace vr
