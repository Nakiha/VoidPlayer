#include "video_renderer/seek_coordinator.h"

namespace vr {

SeekCoordinator::SeekCoordinator(std::chrono::milliseconds paused_hevc_settle_delay)
    : paused_hevc_settle_delay_(paused_hevc_settle_delay) {}

void SeekCoordinator::reset() {
    deferred_paused_hevc_seek_.reset();
    paused_hevc_seek_in_flight_ = false;
    paused_hevc_initial_settle_done_ = false;
    paused_hevc_seek_settle_until_ = Clock::time_point{};
}

bool SeekCoordinator::should_defer_paused_hevc_seek(bool playing,
                                                    bool has_hevc_hw_track,
                                                    int64_t target_pts_us,
                                                    SeekType type) {
    if (playing || !has_hevc_hw_track || type != SeekType::Exact) {
        return false;
    }

    const auto now = Clock::now();
    if (!paused_hevc_seek_in_flight_ && now >= paused_hevc_seek_settle_until_) {
        paused_hevc_seek_in_flight_ = true;
        deferred_paused_hevc_seek_.reset();
        return false;
    }

    deferred_paused_hevc_seek_ = SeekRequest{target_pts_us, type};
    return true;
}

std::optional<SeekRequest> SeekCoordinator::take_deferred_paused_hevc_seek(bool playing) {
    if (playing ||
        !deferred_paused_hevc_seek_.has_value() ||
        paused_hevc_seek_in_flight_ ||
        Clock::now() < paused_hevc_seek_settle_until_) {
        return std::nullopt;
    }

    auto deferred = deferred_paused_hevc_seek_;
    deferred_paused_hevc_seek_.reset();
    paused_hevc_seek_in_flight_ = true;
    return deferred;
}

void SeekCoordinator::mark_paused_hevc_preview_drawn(bool has_hevc_hw_track) {
    if (!has_hevc_hw_track) {
        return;
    }

    if (paused_hevc_seek_in_flight_) {
        paused_hevc_seek_in_flight_ = false;
        paused_hevc_seek_settle_until_ = Clock::now() + paused_hevc_settle_delay_;
        return;
    }

    if (!paused_hevc_initial_settle_done_) {
        paused_hevc_initial_settle_done_ = true;
        paused_hevc_seek_settle_until_ = Clock::now() + paused_hevc_settle_delay_;
    }
}

} // namespace vr
