#include "video_renderer/track/track_step_policy.h"
#include "video_renderer/track/track_present_policy.h"

#include <algorithm>
#include <limits>
#include <vector>

namespace vr {

namespace {
constexpr int64_t kFallbackFrameDurationUs = 33333;
constexpr int64_t kMaxTrustedFrameDurationUs = 100000;

struct FairStepTrackCandidate {
    size_t slot = 0;
    int64_t current_pts_us = 0;
    std::optional<TextureFrame> current_frame;
    std::optional<TextureFrame> next_frame;
    std::optional<TextureFrame> next_next_frame;
    int64_t next_global_pts_us = 0;
    int64_t next_next_global_pts_us = 0;
};

struct FairBackwardStepTrackCandidate {
    size_t slot = 0;
    std::optional<TextureFrame> current_frame;
    std::optional<TextureFrame> previous_frame;
    int64_t current_global_pts_us = 0;
    int64_t previous_global_pts_us = 0;
};

bool collect_fair_step_track_candidate(
    const TrackPipelineManager& tracks,
    size_t slot,
    int64_t current_pts_us,
    const PresentDecision& last_decision,
    FairStepTrackCandidate& out) {
    const auto& track = tracks[slot];
    if (!track || !track->track_buffer) {
        return false;
    }

    out = FairStepTrackCandidate();
    out.slot = slot;
    out.current_pts_us = current_pts_us - track->offset_us;
    if (present_decision_slot_matches_track(last_decision, tracks, slot)) {
        out.current_frame = last_decision.frames[slot];
        out.current_pts_us = out.current_frame->pts_us;
    } else if (auto current = track->track_buffer->peek(0);
               current.has_value()) {
        out.current_frame = current;
        out.current_pts_us = current->pts_us;
    }

    const size_t total = track->track_buffer->total_count();
    for (size_t offset = 0; offset < total; ++offset) {
        auto frame = track->track_buffer->peek(static_cast<int>(offset));
        if (!frame.has_value() || frame->pts_us <= out.current_pts_us) {
            continue;
        }
        if (!out.next_frame.has_value()) {
            out.next_frame = frame;
            out.next_global_pts_us = frame->pts_us + track->offset_us;
            continue;
        }
        out.next_next_frame = frame;
        out.next_next_global_pts_us = frame->pts_us + track->offset_us;
        break;
    }
    return true;
}

bool fair_step_candidate_is_valid_for_track(
    const FairStepTrackCandidate& track,
    int64_t candidate_global_pts_us,
    int64_t max_step_gap_us,
    int* stepped_track_count,
    bool* blocked_by_missing_lookahead) {
    if (!track.next_frame.has_value() ||
        candidate_global_pts_us < track.next_global_pts_us) {
        return true;
    }

    if (max_step_gap_us > 0 &&
        track.next_frame->pts_us - track.current_pts_us > max_step_gap_us) {
        return false;
    }
    if (track.next_next_frame.has_value()) {
        if (candidate_global_pts_us >= track.next_next_global_pts_us) {
            return false;
        }
    } else if (candidate_global_pts_us > track.next_global_pts_us) {
        // Without next-next lookahead, only an exact landing on this track's
        // next frame can prove that the candidate will not skip an unknown
        // intermediate frame.
        if (blocked_by_missing_lookahead) {
            *blocked_by_missing_lookahead = true;
        }
        return false;
    }
    if (stepped_track_count) {
        ++(*stepped_track_count);
    }
    return true;
}

bool collect_fair_backward_step_track_candidate(
    const TrackPipelineManager& tracks,
    size_t slot,
    const PresentDecision& last_decision,
    FairBackwardStepTrackCandidate& out) {
    const auto& track = tracks[slot];
    if (!track || !track->track_buffer) {
        return false;
    }

    out = FairBackwardStepTrackCandidate();
    out.slot = slot;
    if (present_decision_slot_matches_track(last_decision, tracks, slot)) {
        out.current_frame = last_decision.frames[slot];
    } else if (auto current = track->track_buffer->peek(0);
               current.has_value()) {
        out.current_frame = current;
    }
    if (!out.current_frame.has_value()) {
        return true;
    }
    out.current_global_pts_us = out.current_frame->pts_us + track->offset_us;

    constexpr int kBackwardSearchDepth = 8;
    const int negative_count = static_cast<int>(
        std::min<size_t>(
            track->track_buffer->available_retreat_count(),
            kBackwardSearchDepth));
    const int positive_count =
        static_cast<int>(std::min<size_t>(track->track_buffer->total_count(), 8));
    for (int offset = -negative_count; offset < positive_count; ++offset) {
        auto frame = track->track_buffer->peek(offset);
        if (!frame.has_value() ||
            frame->pts_us >= out.current_frame->pts_us) {
            continue;
        }
        if (!out.previous_frame.has_value() ||
            frame->pts_us > out.previous_frame->pts_us) {
            out.previous_frame = frame;
            out.previous_global_pts_us = frame->pts_us + track->offset_us;
        }
    }
    return true;
}

bool fair_backward_candidate_is_valid_for_track(
    const FairBackwardStepTrackCandidate& track,
    int64_t candidate_global_pts_us,
    int* stepped_track_count) {
    if (!track.current_frame.has_value() ||
        candidate_global_pts_us >= track.current_global_pts_us) {
        return true;
    }
    if (!track.previous_frame.has_value() ||
        candidate_global_pts_us < track.previous_global_pts_us) {
        return false;
    }
    if (stepped_track_count) {
        ++(*stepped_track_count);
    }
    return true;
}
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

StepDecisionBuildResult build_step_forward_decision_result(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us,
    int64_t frame_duration_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) {
    StepDecisionBuildResult result;
    decision = PresentDecision();
    decision.current_pts_us = current_pts_us;
    const int64_t max_step_gap_us =
        frame_duration_us + frame_duration_us / 2 + 2000;

    bool any_active = false;
    std::vector<FairStepTrackCandidate> track_candidates;
    std::vector<int64_t> target_candidates;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            decision.frames[i] = std::nullopt;
            continue;
        }
        any_active = true;
        if (!tracks[i]->track_buffer) {
            decision = PresentDecision();
            return result;
        }

        FairStepTrackCandidate candidate;
        if (!collect_fair_step_track_candidate(
                tracks, i, current_pts_us, last_decision, candidate)) {
            decision = PresentDecision();
            return result;
        }
        if (candidate.next_frame.has_value()) {
            target_candidates.push_back(candidate.next_global_pts_us);
        }
        track_candidates.push_back(candidate);
    }

    if (!any_active || target_candidates.empty()) {
        decision = PresentDecision();
        return result;
    }

    std::sort(target_candidates.begin(), target_candidates.end());
    target_candidates.erase(
        std::unique(target_candidates.begin(), target_candidates.end()),
        target_candidates.end());

    std::optional<int64_t> selected_target;
    int selected_step_count = 0;
    for (const int64_t candidate_target : target_candidates) {
        bool valid = true;
        int step_count = 0;
        for (const auto& track_candidate : track_candidates) {
            bool blocked_by_missing_lookahead = false;
            if (!fair_step_candidate_is_valid_for_track(
                    track_candidate,
                    candidate_target,
                    max_step_gap_us,
                    &step_count,
                    &blocked_by_missing_lookahead)) {
                if (blocked_by_missing_lookahead) {
                    result.needs_lookahead = true;
                }
                valid = false;
                break;
            }
        }
        if (!valid || step_count == 0) {
            continue;
        }
        if (!selected_target.has_value() ||
            step_count > selected_step_count ||
            (step_count == selected_step_count &&
             candidate_target < *selected_target)) {
            selected_target = candidate_target;
            selected_step_count = step_count;
        }
    }

    if (!selected_target.has_value()) {
        decision = PresentDecision();
        return result;
    }

    decision.current_pts_us = *selected_target;
    for (const auto& track_candidate : track_candidates) {
        const auto& track = tracks[track_candidate.slot];
        if (!track) {
            decision = PresentDecision();
            result.has_decision = false;
            return result;
        }

        std::optional<TextureFrame> selected_frame;
        if (track_candidate.next_frame.has_value() &&
            *selected_target >= track_candidate.next_global_pts_us) {
            selected_frame = track_candidate.next_frame;
        } else {
            selected_frame = track_candidate.current_frame;
        }
        if (!selected_frame.has_value()) {
            decision = PresentDecision();
            result.has_decision = false;
            return result;
        }
        decision.frames[track_candidate.slot] = selected_frame;
        set_present_decision_track_identity(
            decision, track_candidate.slot, *track);
    }

    decision.should_present = any_active;
    result.has_decision = decision.should_present;
    return result;
}

bool build_step_forward_decision(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us,
    int64_t frame_duration_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) {
    return build_step_forward_decision_result(
        tracks, current_pts_us, frame_duration_us, last_decision, decision)
        .has_decision;
}

StepDecisionApplication apply_step_forward_decision(
    TrackPipelineManager& tracks,
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision) {
    StepDecisionApplication result;
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
    result.clock_target_us = decision.current_pts_us;
    result.has_clock_target = true;
    return result;
}

bool build_step_backward_decision(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) {
    decision = PresentDecision();
    decision.current_pts_us = current_pts_us;

    bool any_active = false;
    std::vector<FairBackwardStepTrackCandidate> track_candidates;
    std::vector<int64_t> target_candidates;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            decision.frames[i] = std::nullopt;
            continue;
        }
        any_active = true;
        FairBackwardStepTrackCandidate candidate;
        if (!collect_fair_backward_step_track_candidate(
                tracks, i, last_decision, candidate)) {
            decision = PresentDecision();
            return false;
        }
        if (!candidate.current_frame.has_value()) {
            decision = PresentDecision();
            return false;
        }
        if (candidate.previous_frame.has_value()) {
            target_candidates.push_back(candidate.previous_global_pts_us);
        }
        track_candidates.push_back(candidate);
    }

    if (!any_active || target_candidates.empty()) {
        decision = PresentDecision();
        return false;
    }

    std::sort(target_candidates.begin(), target_candidates.end());
    target_candidates.erase(
        std::unique(target_candidates.begin(), target_candidates.end()),
        target_candidates.end());

    std::optional<int64_t> selected_target;
    int selected_step_count = 0;
    for (const int64_t candidate_target : target_candidates) {
        bool valid = true;
        int step_count = 0;
        for (const auto& track_candidate : track_candidates) {
            if (!fair_backward_candidate_is_valid_for_track(
                    track_candidate, candidate_target, &step_count)) {
                valid = false;
                break;
            }
        }
        if (!valid || step_count == 0) {
            continue;
        }
        if (!selected_target.has_value() ||
            step_count > selected_step_count ||
            (step_count == selected_step_count &&
             candidate_target > *selected_target)) {
            selected_target = candidate_target;
            selected_step_count = step_count;
        }
    }

    if (!selected_target.has_value()) {
        decision = PresentDecision();
        return false;
    }

    decision.current_pts_us = *selected_target;
    for (const auto& track_candidate : track_candidates) {
        const auto& track = tracks[track_candidate.slot];
        if (!track) {
            decision = PresentDecision();
            return false;
        }
        std::optional<TextureFrame> selected_frame = track_candidate.current_frame;
        if (*selected_target < track_candidate.current_global_pts_us) {
            selected_frame = track_candidate.previous_frame;
        }
        if (!selected_frame.has_value()) {
            decision = PresentDecision();
            return false;
        }
        decision.frames[track_candidate.slot] = selected_frame;
        set_present_decision_track_identity(
            decision, track_candidate.slot, *track);
    }

    decision.should_present = true;
    return true;
}

StepDecisionApplication apply_step_backward_decision(
    TrackPipelineManager& tracks,
    const PresentDecision& decision) {
    StepDecisionApplication result;
    result.reference_slot = tracks.first_active_slot();
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i] || !tracks[i]->track_buffer ||
            !decision.frames[i].has_value()) {
            continue;
        }
        auto& buffer = tracks[i]->track_buffer;
        while (true) {
            auto frame = buffer->peek(0);
            if (!frame.has_value() ||
                frame->pts_us <= decision.frames[i]->pts_us) {
                break;
            }
            if (!buffer->retreat()) {
                break;
            }
        }
    }
    if (result.reference_slot < 0) {
        return result;
    }
    const auto slot = static_cast<size_t>(result.reference_slot);
    if (!tracks[slot] || !decision.frames[slot].has_value()) {
        return result;
    }
    result.presented_pts_us = decision.frames[slot]->pts_us;
    result.clock_target_us = decision.current_pts_us;
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
    result.frame_duration_us = compute_min_current_frame_duration_us(tracks);
    const int64_t forward_slop_us = std::max<int64_t>(2000, result.frame_duration_us / 2);
    constexpr int64_t kTrustedVisibleLookaheadUs = 500000;
    const auto apply_candidate_base = [&](int64_t candidate_us, bool trusted_visible) {
        if (trusted_visible &&
            candidate_us >= clock_pts_us &&
            candidate_us <= clock_pts_us + kTrustedVisibleLookaheadUs) {
            result.base_pts_us = candidate_us;
            return;
        }
        if (candidate_us <= clock_pts_us + forward_slop_us) {
            result.base_pts_us = std::max(candidate_us, clock_pts_us);
        }
    };
    result.reference_slot = tracks.first_active_slot();
    if (result.reference_slot >= 0) {
        const auto slot = static_cast<size_t>(result.reference_slot);
        const auto& track = tracks[slot];
        if (track) {
            if (present_decision_slot_matches_track(last_decision, tracks, slot)) {
                apply_candidate_base(
                    last_decision.frames[slot]->pts_us + track->offset_us,
                    true);
            } else if (track->track_buffer) {
                auto frame = track->track_buffer->peek(0);
                if (frame.has_value()) {
                    apply_candidate_base(frame->pts_us + track->offset_us, false);
                }
            }
        }
    }

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
    int64_t clock_pts_us,
    const PresentDecision& last_decision) {
    StepBackwardExactSeekTarget result;
    result.clock_pts_us = clock_pts_us;
    result.base_pts_us = clock_pts_us;
    result.frame_duration_us = compute_min_current_frame_duration_us(tracks);
    const int64_t visible_slop_us =
        std::max<int64_t>(2000, result.frame_duration_us + 2000);
    constexpr int64_t kTrustedVisibleLookbehindUs = 500000;
    const auto apply_candidate_base = [&](int64_t candidate_us, bool trusted_visible) {
        if ((trusted_visible &&
             candidate_us <= clock_pts_us &&
             candidate_us >= std::max<int64_t>(0, clock_pts_us - kTrustedVisibleLookbehindUs)) ||
            (candidate_us >= std::max<int64_t>(0, clock_pts_us - visible_slop_us) &&
             candidate_us <= clock_pts_us + visible_slop_us)) {
            result.base_pts_us = candidate_us;
        }
    };

    result.reference_slot = tracks.first_active_slot();
    if (result.reference_slot >= 0) {
        const auto slot = static_cast<size_t>(result.reference_slot);
        const auto& track = tracks[slot];
        if (track) {
            if (present_decision_slot_matches_track(last_decision, tracks, slot)) {
                apply_candidate_base(
                    last_decision.frames[slot]->pts_us + track->offset_us,
                    true);
            } else if (track->track_buffer) {
                auto frame = track->track_buffer->peek(0);
                if (frame.has_value()) {
                    apply_candidate_base(frame->pts_us + track->offset_us, false);
                }
            }
        }
    }

    const int64_t target =
        result.base_pts_us - result.frame_duration_us - 1000;
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
        } else if (present_decision_slot_matches_track(last_decision, tracks, i)) {
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
