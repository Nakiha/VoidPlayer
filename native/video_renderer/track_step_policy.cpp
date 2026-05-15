#include "video_renderer/track_step_policy.h"

#include <algorithm>
#include <limits>

namespace vr {

namespace {
constexpr int64_t kFallbackFrameDurationUs = 33333;
constexpr int64_t kMaxTrustedFrameDurationUs = 100000;
}

bool has_buffering_track(const TrackPipelineManager& tracks) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i] || !tracks[i]->track_buffer) {
            continue;
        }
        if (tracks[i]->track_buffer->state() == TrackState::Buffering) {
            return true;
        }
    }
    return false;
}

void apply_track_video_decode_pause_state(
    TrackPipelineManager& tracks,
    bool paused,
    std::function<void(size_t slot, TrackPipeline& track, bool paused)>
        set_decode_paused) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        if (set_decode_paused) {
            set_decode_paused(i, *tracks[i], paused);
        }
    }
}

bool retreat_tracks_if_all_can_retreat(TrackPipelineManager& tracks) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        if (!tracks[i]->track_buffer || !tracks[i]->track_buffer->can_retreat()) {
            return false;
        }
    }

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i] || !tracks[i]->track_buffer) {
            continue;
        }
        tracks[i]->track_buffer->retreat();
    }
    return true;
}

int64_t compute_min_current_frame_duration_us(
    const TrackPipelineManager& tracks) {
    int64_t min_duration_us = std::numeric_limits<int64_t>::max();
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i] || !tracks[i]->track_buffer) {
            continue;
        }
        auto frame = tracks[i]->track_buffer->peek(0);
        if (frame.has_value() && frame->duration_us > 0) {
            min_duration_us = std::min(min_duration_us, frame->duration_us);
        }
    }
    if (min_duration_us != std::numeric_limits<int64_t>::max() &&
        min_duration_us <= kMaxTrustedFrameDurationUs) {
        return min_duration_us;
    }
    return kFallbackFrameDurationUs;
}

bool build_step_forward_decision(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us,
    int64_t frame_duration_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) {
    decision = PresentDecision();
    decision.current_pts_us = current_pts_us;
    const int64_t max_step_gap_us =
        frame_duration_us + frame_duration_us / 2 + 2000;

    bool any_active = false;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            decision.frames[i] = std::nullopt;
            continue;
        }
        any_active = true;
        if (!tracks[i]->track_buffer) {
            decision = PresentDecision();
            return false;
        }

        int64_t base_pts = current_pts_us - tracks[i]->offset_us;
        if (last_decision.frames[i].has_value()) {
            base_pts = last_decision.frames[i]->pts_us;
        } else if (auto current = tracks[i]->track_buffer->peek(0);
                   current.has_value()) {
            base_pts = current->pts_us;
        }

        std::optional<TextureFrame> best;
        auto& buffer = tracks[i]->track_buffer;
        const size_t total = buffer->total_count();
        for (size_t offset = 0; offset < total; ++offset) {
            auto frame = buffer->peek(static_cast<int>(offset));
            if (!frame.has_value() || frame->pts_us <= base_pts) {
                continue;
            }
            if (!best.has_value() || frame->pts_us < best->pts_us) {
                best = frame;
            }
        }

        if (!best.has_value()) {
            decision = PresentDecision();
            return false;
        }
        if (frame_duration_us > 0 && best->pts_us - base_pts > max_step_gap_us) {
            decision = PresentDecision();
            return false;
        }
        decision.frames[i] = best;
    }

    decision.should_present = any_active;
    return decision.should_present;
}

StepForwardDecisionApplication apply_step_forward_decision(
    TrackPipelineManager& tracks,
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision) {
    StepForwardDecisionApplication result;
    discard_step_forward_consumed_frames(
        tracks, current_pts_us, decision, last_decision);

    result.reference_slot = tracks.first_active_slot();
    if (result.reference_slot < 0) {
        return result;
    }

    const auto slot = static_cast<size_t>(result.reference_slot);
    if (!tracks[slot] || !decision.frames[slot].has_value()) {
        return result;
    }

    result.presented_pts_us = decision.frames[slot]->pts_us;
    result.clock_target_us =
        result.presented_pts_us + tracks[slot]->offset_us;
    result.has_clock_target = true;
    return result;
}

StepForwardExactSeekTarget choose_step_forward_exact_seek_target(
    const TrackPipelineManager& tracks,
    int64_t clock_pts_us,
    int64_t cached_duration_us,
    const PresentDecision& last_decision) {
    StepForwardExactSeekTarget result;
    result.clock_pts_us = clock_pts_us;
    result.base_pts_us = clock_pts_us;
    result.reference_slot = tracks.first_active_slot();
    if (result.reference_slot >= 0) {
        const auto slot = static_cast<size_t>(result.reference_slot);
        const auto& track = tracks[slot];
        if (track) {
            if (last_decision.frames[slot].has_value()) {
                result.base_pts_us =
                    last_decision.frames[slot]->pts_us + track->offset_us;
            } else if (track->track_buffer) {
                auto frame = track->track_buffer->peek(0);
                if (frame.has_value()) {
                    result.base_pts_us = frame->pts_us + track->offset_us;
                }
            }
        }
    }

    result.frame_duration_us = compute_min_current_frame_duration_us(tracks);
    result.target_pts_us =
        result.base_pts_us + result.frame_duration_us + 1000;
    if (cached_duration_us > 0 && result.target_pts_us > cached_duration_us) {
        result.target_pts_us = cached_duration_us;
        result.clamped_to_duration = true;
    }
    return result;
}

StepBackwardExactSeekTarget choose_step_backward_exact_seek_target(
    const TrackPipelineManager& tracks,
    int64_t clock_pts_us) {
    StepBackwardExactSeekTarget result;
    result.clock_pts_us = clock_pts_us;
    result.frame_duration_us = compute_min_current_frame_duration_us(tracks);
    const int64_t target =
        clock_pts_us - result.frame_duration_us - 1000;
    result.target_pts_us = std::max(int64_t(0), target);
    result.clamped_to_zero = result.target_pts_us != target;
    return result;
}

void discard_step_forward_consumed_frames(
    TrackPipelineManager& tracks,
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i] || !tracks[i]->track_buffer) {
            continue;
        }

        int64_t keep_after_pts = current_pts_us - tracks[i]->offset_us;
        if (decision.frames[i].has_value()) {
            keep_after_pts = decision.frames[i]->pts_us;
        } else if (last_decision.frames[i].has_value()) {
            keep_after_pts = last_decision.frames[i]->pts_us;
        }

        auto& buffer = tracks[i]->track_buffer;
        while (true) {
            auto frame = buffer->peek(0);
            if (!frame.has_value() || frame->pts_us > keep_after_pts) {
                break;
            }
            if (!buffer->advance()) {
                break;
            }
        }
    }
}

} // namespace vr
