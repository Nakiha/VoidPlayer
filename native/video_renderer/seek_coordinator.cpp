#include "video_renderer/seek_coordinator.h"

namespace vr {

HevcSeekRecreateDecision choose_hevc_seek_recreate(
    const HevcSeekRecreateInput& input) {
    HevcSeekRecreateDecision decision;
    if (!input.is_hevc_hw_seek) {
        return decision;
    }

    decision.coalescing_transition = input.seek_transition_active;
    decision.error_if_recreate_not_applied =
        !input.paused_seek && !input.seek_transition_active;
    decision.should_recreate_pipeline =
        (!input.paused_seek && !input.seek_transition_active) ||
        (input.paused_seek &&
         input.seek_type != SeekType::Exact &&
         !input.recreated_for_paused_hevc_seek &&
         (!input.seek_transition_active || input.force_recreate_paused_hevc));
    return decision;
}

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
