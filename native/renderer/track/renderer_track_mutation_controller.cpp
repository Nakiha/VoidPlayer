#include "renderer/track/renderer_track_mutation_controller.h"

#include "renderer/seek/renderer_seek_log_policy.h"
#include "renderer/track/renderer_track_registry.h"
#include "renderer/track/track_lifecycle.h"
#include "renderer/track/track_step_policy.h"

namespace vr {

RendererTrackMutationController::RendererTrackMutationController(
    RendererTrackRegistry& registry)
    : registry_(registry) {}

bool RendererTrackMutationController::configure_and_start_pipeline(
    TrackPipeline& pipeline,
    const TrackPipelineStartConfig& config,
    const TrackPipelineStartHooks& hooks,
    const char* log_context) const {
    return configure_and_start_track_pipeline(
        pipeline, config, hooks, log_context);
}

void RendererTrackMutationController::stop_detached_pipeline(
    size_t slot,
    std::unique_ptr<TrackPipeline>& track) const {
    stop_detached_track_pipeline(slot, track);
}

void RendererTrackMutationController::set_video_decode_paused(
    bool paused,
    const std::function<void(size_t slot, TrackPipeline& track, bool paused)>&
        set_decode_paused) {
    apply_track_video_decode_pause_state(
        registry_.mutable_tracks_for_mutation(), paused, set_decode_paused);
}

void RendererTrackMutationController::set_decode_paused_for_all(
    bool paused,
    const TrackDecodePauseHooks& hooks) {
    apply_track_decode_pause_state(
        registry_.mutable_tracks_for_mutation(), paused, hooks);
}

void RendererTrackMutationController::apply_playback_decode_state(
    bool playback_active,
    const TrackPlaybackDecodeStateHooks& hooks) {
    vr::apply_track_playback_decode_state(
        registry_.mutable_tracks_for_mutation(), playback_active, hooks);
}

bool RendererTrackMutationController::apply_track_offset(
    int file_id,
    int64_t offset_us,
    const TrackOffsetMutationHooks& hooks) {
    auto& tracks = registry_.mutable_tracks_for_mutation();
    const int slot = registry_.find_slot_by_file_id(file_id);
    if (slot < 0 || !tracks[static_cast<size_t>(slot)]) {
        return false;
    }
    apply_track_offset_mutation(
        *tracks[static_cast<size_t>(slot)],
        static_cast<size_t>(slot),
        offset_us,
        hooks);
    return true;
}

StepDecisionApplication RendererTrackMutationController::apply_step_forward_decision(
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision) {
    return vr::apply_step_forward_decision(
        registry_.mutable_tracks_for_mutation(),
        current_pts_us,
        decision,
        last_decision);
}

void RendererTrackMutationController::discard_step_forward_consumed_frames(
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision) {
    vr::discard_step_forward_consumed_frames(
        registry_.mutable_tracks_for_mutation(),
        current_pts_us,
        decision,
        last_decision);
}

StepDecisionApplication RendererTrackMutationController::apply_step_backward_decision(
    const PresentDecision& decision) {
    return vr::apply_step_backward_decision(
        registry_.mutable_tracks_for_mutation(), decision);
}

bool RendererTrackMutationController::can_commit_add(size_t slot) const {
    const auto& tracks = registry_.tracks_for_snapshot();
    return slot < kMaxTracks && !tracks[slot];
}

TrackPipeline* RendererTrackMutationController::commit_new_track(
    size_t slot,
    std::unique_ptr<TrackPipeline> pipeline,
    const TrackAddCommitHooks& hooks) {
    return commit_new_track_pipeline(
        registry_.mutable_tracks_for_mutation(),
        slot,
        std::move(pipeline),
        hooks);
}

RendererTrackDetachResult
RendererTrackMutationController::detach_and_compact_by_file_id(
    int file_id,
    const RendererTrackDetachHooks& hooks) {
    RendererTrackDetachResult result;
    auto& tracks = registry_.mutable_tracks_for_mutation();
    const int slot = registry_.find_slot_by_file_id(file_id);
    if (slot < 0) {
        return result;
    }
    result.removed = true;
    result.slot = slot;
    const auto detached_slot = static_cast<size_t>(slot);
    if (hooks.clear_slot && tracks[detached_slot]) {
        hooks.clear_slot(detached_slot, *tracks[detached_slot]);
    }
    result.detached_track = std::move(tracks[detached_slot]);
    if (result.detached_track && result.detached_track->demux_thread) {
        result.detached_track->demux_thread->set_seek_callback({});
        result.detached_track->demux_thread->set_error_callback({});
    }

    tracks.compact_from(
        detached_slot, [&](size_t from, size_t to, TrackPipeline& track) {
            if (hooks.move_slot) {
                hooks.move_slot(from, to, track);
            }
        });
    registry_.recompute_cached_duration();
    result.remaining = registry_.count();
    return result;
}

RendererTrackRecreateDetachResult
RendererTrackMutationController::detach_for_recreate(
    size_t slot,
    const RendererTrackRecreateDetachHooks& hooks) {
    RendererTrackRecreateDetachResult result;
    auto& tracks = registry_.mutable_tracks_for_mutation();
    if (slot >= kMaxTracks || !tracks[slot]) {
        return result;
    }
    auto& current = tracks[slot];
    result.detached = true;
    result.slot = slot;
    result.file_path = current->file_path;
    result.file_id = current->file_id;
    result.offset_us = current->offset_us;
    result.use_hardware_decode = current->use_hardware_decode;
    result.replacement_generation = registry_.allocate_generation();
    if (hooks.clear_slot) {
        hooks.clear_slot(slot, *current);
    }
    result.detached_track = std::move(current);
    if (result.detached_track && result.detached_track->demux_thread) {
        result.detached_track->demux_thread->set_seek_callback({});
        result.detached_track->demux_thread->set_error_callback({});
    }
    return result;
}

bool RendererTrackMutationController::can_commit_recreated_track(size_t slot) const {
    return can_commit_add(slot);
}

TrackPipeline* RendererTrackMutationController::commit_recreated_track(
    size_t slot,
    std::unique_ptr<TrackPipeline> pipeline,
    const TrackAddCommitHooks& hooks) {
    return commit_new_track(slot, std::move(pipeline), hooks);
}

std::vector<RendererTrackSeekApplicationResult>
RendererTrackMutationController::apply_seek_to_all(
    int64_t target_pts_us,
    SeekType type,
    bool playing,
    bool force_recreate_paused_hevc,
    const RendererTrackSeekHooks& hooks) {
    std::vector<RendererTrackSeekApplicationResult> results;
    auto& tracks = registry_.mutable_tracks_for_mutation();
    for (size_t i = 0; i < kMaxTracks; ++i) {
        const TrackSeekSlotApplicationHooks seek_hooks{
            TrackSeekPreparationHooks{
                hooks.set_audio_decode_paused,
                [&hooks, i]() {
                    if (hooks.reset_presenter_track) {
                        hooks.reset_presenter_track(i);
                    }
                },
            },
            hooks.recreate_pipeline_for_seek,
        };
        auto seek_result = apply_track_seek_to_slot(
            tracks, i, target_pts_us, type, playing,
            force_recreate_paused_hevc, seek_hooks);
        if (!seek_result.slot_present) {
            continue;
        }

        RendererTrackSeekApplicationResult result;
        result.slot = i;
        result.seek = std::move(seek_result);
        if (result.seek.execution.applied_seek &&
            tracks[i] &&
            tracks[i]->track_buffer) {
            result.buffered_frames_after =
                tracks[i]->track_buffer->total_count();
        }
        results.push_back(std::move(result));
    }
    return results;
}

bool RendererTrackMutationController::apply_seek_to_all_and_log(
    int64_t target_pts_us,
    SeekType type,
    bool playing,
    bool force_recreate_paused_hevc,
    const RendererTrackSeekHooks& hooks) {
    const auto seek_results = apply_seek_to_all(
        target_pts_us,
        type,
        playing,
        force_recreate_paused_hevc,
        hooks);
    return log_renderer_track_seek_application_results(seek_results);
}

} // namespace vr
