#include "video_renderer/track_buffer_budget.h"

namespace vr {

bool is_high_resolution_track(const DemuxStats& stats,
                              const NativeResourceBudget& budget) {
    if (stats.width <= 0 || stats.height <= 0) {
        return false;
    }
    return static_cast<size_t>(stats.width) * static_cast<size_t>(stats.height) >=
        budget.high_resolution_track_pixels;
}

TrackBufferBudgetDecision choose_track_buffer_budget(
    const DemuxStats& stats,
    bool hw_decode,
    const NativeResourceBudget& budget) {
    const bool high_resolution = is_high_resolution_track(stats, budget);
    const size_t forward_depth =
        hw_decode && high_resolution
            ? budget.high_resolution_hardware_track_forward_depth
            : budget.default_track_forward_depth;
    return TrackBufferBudgetDecision{
        forward_depth,
        budget.default_track_backward_depth,
        high_resolution,
    };
}

} // namespace vr
