#include "video_renderer/track/track_perf_baseline.h"

namespace vr {

void TrackPerfBaselineTracker::reset(Clock::time_point start_time) {
    start_time_ = start_time;
    baseline_frames_.fill(0);
}

double TrackPerfBaselineTracker::elapsed_seconds(Clock::time_point now) const {
    return std::chrono::duration<double>(now - start_time_).count();
}

bool TrackPerfBaselineTracker::should_rotate(double elapsed_seconds) const {
    return elapsed_seconds > 0.5;
}

void TrackPerfBaselineTracker::rotate_timer(Clock::time_point start_time) {
    start_time_ = start_time;
}

uint64_t TrackPerfBaselineTracker::baseline_frames(size_t slot) const {
    if (slot >= kMaxTracks) {
        return 0;
    }
    return baseline_frames_[slot];
}

void TrackPerfBaselineTracker::update_baseline_frames(size_t slot,
                                                       uint64_t frames) {
    if (slot >= kMaxTracks) {
        return;
    }
    baseline_frames_[slot] = frames;
}

} // namespace vr
