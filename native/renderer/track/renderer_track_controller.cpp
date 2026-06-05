#include "renderer/track/renderer_track_controller.h"

#include "renderer/seek/renderer_seek_log_policy.h"
#include "renderer/track/track_lifecycle.h"
#include "renderer/track/track_preroll_policy.h"

#include <algorithm>

namespace vr {

std::unique_ptr<TrackPipeline> RendererTrackController::create_pipeline(
    const std::string& path,
    bool hw_decode,
    RenderBackendKind render_backend,
    const SeekRequest* initial_seek) const {
    TrackPipelineOpenOptions options;
    options.render_backend = render_backend;
    return factory_.create_opened_pipeline(path, hw_decode, initial_seek, options);
}

bool RendererTrackController::configure_and_start_pipeline(
    TrackPipeline& pipeline,
    const TrackPipelineStartConfig& config,
    const TrackPipelineStartHooks& hooks,
    const char* log_context) const {
    return configure_and_start_track_pipeline(
        pipeline, config, hooks, log_context);
}

void RendererTrackController::stop_detached_pipeline(
    size_t slot,
    std::unique_ptr<TrackPipeline>& track) const {
    stop_detached_track_pipeline(slot, track);
}

void RendererTrackController::assign_missing_generations() {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i] && tracks_[i]->generation == 0) {
            tracks_[i]->generation = allocate_generation();
        }
    }
}

int RendererTrackController::first_active_slot() const {
    return tracks_.first_active_slot();
}

int RendererTrackController::find_empty_slot() const {
    return tracks_.find_empty_slot();
}

int RendererTrackController::find_slot_by_file_id(int file_id) const {
    return tracks_.find_slot_by_file_id(file_id);
}

int RendererTrackController::audio_info_slot(int preferred_file_id) const {
    if (preferred_file_id >= 0) {
        const int slot = find_slot_by_file_id(preferred_file_id);
        if (slot >= 0 && tracks_[static_cast<size_t>(slot)] &&
            tracks_[static_cast<size_t>(slot)]->demux_thread) {
            const auto& stats =
                tracks_[static_cast<size_t>(slot)]->demux_thread->stats();
            if (stats.audio_stream_index >= 0 && stats.audio_codec_params) {
                return slot;
            }
        }
    }

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i] || !tracks_[i]->demux_thread) {
            continue;
        }
        const auto& stats = tracks_[i]->demux_thread->stats();
        if (stats.audio_stream_index >= 0 && stats.audio_codec_params) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int RendererTrackController::audio_sample_rate_for_slot(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks) ||
        !tracks_[static_cast<size_t>(slot)] ||
        !tracks_[static_cast<size_t>(slot)]->demux_thread) {
        return 0;
    }
    return tracks_[static_cast<size_t>(slot)]->demux_thread->stats().sample_rate;
}

int RendererTrackController::audio_channels_for_slot(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks) ||
        !tracks_[static_cast<size_t>(slot)] ||
        !tracks_[static_cast<size_t>(slot)]->demux_thread) {
        return 0;
    }
    return tracks_[static_cast<size_t>(slot)]->demux_thread->stats().channels;
}

bool RendererTrackController::uses_hardware_codec(AVCodecID codec_id) const {
    return any_track_uses_hardware_codec(tracks_, codec_id);
}

int64_t RendererTrackController::min_current_frame_duration_us() const {
    return compute_min_current_frame_duration_us(tracks_);
}

bool RendererTrackController::has_slot(int slot) const {
    return slot >= 0 &&
           slot < static_cast<int>(kMaxTracks) &&
           tracks_[static_cast<size_t>(slot)] != nullptr;
}

std::pair<int, int> RendererTrackController::dimensions_for_slot(int slot) const {
    if (!has_slot(slot)) {
        return {0, 0};
    }
    const auto& track = tracks_[static_cast<size_t>(slot)];
    return {track->video_width, track->video_height};
}

std::vector<TrackInfo> RendererTrackController::infos() const {
    return snapshot_track_infos(tracks_);
}

std::vector<TrackPerfStats> RendererTrackController::perf_stats(
    const PresentDecision& last_decision,
    std::chrono::steady_clock::time_point now) {
    const double elapsed_s = perf_baseline_tracker_.elapsed_seconds(now);
    const bool should_rotate = perf_baseline_tracker_.should_rotate(elapsed_s);
    PresentDecision filtered_last_decision = last_decision;
    filter_present_decision_against_tracks(filtered_last_decision, tracks_);
    const auto snapshot = snapshot_track_perf_stats_collection(
        tracks_, filtered_last_decision, perf_baseline_tracker_, elapsed_s);

    if (should_rotate) {
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!snapshot.frames_decoded_by_slot[i].has_value()) {
                continue;
            }
            perf_baseline_tracker_.update_baseline_frames(
                i, *snapshot.frames_decoded_by_slot[i]);
        }
        perf_baseline_tracker_.rotate_timer(now);
    }
    return snapshot.stats;
}

TrackGpuMemoryStatsCollectionResult RendererTrackController::gpu_memory_stats(
    const std::array<uint64_t, kMaxTracks>& presenter_copy_texture_bytes_by_slot) const {
    return snapshot_track_gpu_memory_stats_collection(
        tracks_, presenter_copy_texture_bytes_by_slot);
}

LayoutTrackGeometryList RendererTrackController::layout_track_geometry() const {
    return snapshot_layout_track_geometry(tracks_);
}

void RendererTrackController::populate_draw_tracks(
    RendererDrawTrackSnapshotList& out) const {
    out = {};
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) {
            continue;
        }
        auto& track = out[i];
        track.active = true;
        track.file_id = tracks_[i]->file_id;
        track.generation = tracks_[i]->generation;
        track.offset_us = tracks_[i]->offset_us;
        track.video_width = tracks_[i]->video_width;
        track.video_height = tracks_[i]->video_height;
        track.video_aspect = tracks_[i]->video_aspect;
    }
}

std::vector<RendererLayoutTrackReference>
RendererTrackController::layout_track_references() const {
    std::vector<RendererLayoutTrackReference> refs;
    refs.reserve(count());
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) {
            continue;
        }
        refs.push_back(RendererLayoutTrackReference{
            tracks_[i]->file_id,
            static_cast<int>(i),
        });
    }
    return refs;
}

bool RendererTrackController::has_active_tracks() const {
    return tracks_.has_active_tracks();
}

size_t RendererTrackController::count() const {
    return tracks_.count();
}

bool RendererTrackController::has_preroll_blocking_track() const {
    return vr::has_preroll_blocking_track(tracks_);
}

bool RendererTrackController::has_buffering_track() const {
    return vr::has_buffering_track(tracks_);
}

std::vector<RenderLoopTrackDiagnosticSnapshot>
RendererTrackController::render_loop_diagnostics() const {
    return snapshot_render_loop_track_diagnostics(tracks_);
}

void RendererTrackController::set_video_decode_paused(
    bool paused,
    const std::function<void(size_t slot, TrackPipeline& track, bool paused)>&
        set_decode_paused) {
    apply_track_video_decode_pause_state(tracks_, paused, set_decode_paused);
}

void RendererTrackController::set_decode_paused_for_all(
    bool paused,
    const TrackDecodePauseHooks& hooks) {
    apply_track_decode_pause_state(tracks_, paused, hooks);
}

void RendererTrackController::apply_playback_decode_state(
    bool playback_active,
    const TrackPlaybackDecodeStateHooks& hooks) {
    vr::apply_track_playback_decode_state(tracks_, playback_active, hooks);
}

bool RendererTrackController::apply_track_offset(
    int file_id,
    int64_t offset_us,
    const TrackOffsetMutationHooks& hooks) {
    const int slot = find_slot_by_file_id(file_id);
    if (slot < 0 || !tracks_[static_cast<size_t>(slot)]) {
        return false;
    }
    apply_track_offset_mutation(
        *tracks_[static_cast<size_t>(slot)],
        static_cast<size_t>(slot),
        offset_us,
        hooks);
    return true;
}

StepDecisionBuildResult RendererTrackController::build_step_forward_decision(
    int64_t current_pts_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    return build_step_forward_decision_result(
        tracks_,
        current_pts_us,
        compute_min_current_frame_duration_us(tracks_),
        last_decision,
        decision);
}

StepDecisionApplication RendererTrackController::apply_step_forward_decision(
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision) {
    return vr::apply_step_forward_decision(
        tracks_, current_pts_us, decision, last_decision);
}

void RendererTrackController::discard_step_forward_consumed_frames(
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision) {
    vr::discard_step_forward_consumed_frames(
        tracks_, current_pts_us, decision, last_decision);
}

StepForwardExactSeekTarget
RendererTrackController::choose_step_forward_exact_seek_target(
    int64_t clock_pts_us,
    const PresentDecision& last_decision) const {
    return vr::choose_step_forward_exact_seek_target(
        tracks_, clock_pts_us, cached_duration_us_, last_decision);
}

bool RendererTrackController::build_step_backward_decision(
    int64_t current_pts_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    return vr::build_step_backward_decision(
        tracks_, current_pts_us, last_decision, decision);
}

StepDecisionApplication RendererTrackController::apply_step_backward_decision(
    const PresentDecision& decision) {
    return vr::apply_step_backward_decision(tracks_, decision);
}

StepBackwardExactSeekTarget
RendererTrackController::choose_step_backward_exact_seek_target(
    int64_t clock_pts_us,
    const PresentDecision& last_decision) const {
    return vr::choose_step_backward_exact_seek_target(
        tracks_, clock_pts_us, last_decision);
}

void RendererTrackController::apply_carry_forward(
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    apply_present_carry_forward(tracks_, last_decision, decision);
}

void RendererTrackController::filter_present_decision(PresentDecision& decision) const {
    filter_present_decision_against_tracks(decision, tracks_);
}

std::vector<SeekPreviewPresentedTrackEvent>
RendererTrackController::collect_seek_preview_presented_events(
    const PresentDecision& decision,
    int64_t request_id,
    int64_t target_pts_us) const {
    return collect_seek_preview_presented_track_events(
        tracks_, decision, request_id, target_pts_us);
}

std::vector<LayoutTrackGeometryUpdate>
RendererTrackController::update_layout_track_geometry_from_decision(
    const PresentDecision& decision) {
    return vr::update_layout_track_geometry_from_decision(tracks_, decision);
}

EmptyBufferEofClamp RendererTrackController::empty_buffer_eof_clamp(
    const PresentDecision& last_decision) const {
    return compute_empty_buffer_eof_clamp(tracks_, last_decision);
}

std::optional<int64_t> RendererTrackController::next_frame_event_pts_us(
    int64_t current_pts_us) const {
    return compute_next_frame_event_pts_us(tracks_, current_pts_us);
}

RendererPausedCachedDecision RendererTrackController::paused_cached_decision(
    const PresentDecision& last_decision) const {
    RendererPausedCachedDecision result;
    PresentDecision decision = last_decision;
    filter_present_decision_against_tracks(decision, tracks_);
    if (!present_decision_has_frame(decision)) {
        return result;
    }

    const auto available = build_available_paused_frame_snapshot(tracks_);
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!decision.frames[i].has_value() &&
            available.decision.frames[i].has_value()) {
            decision.frames[i] = available.decision.frames[i];
            decision.file_ids[i] = available.decision.file_ids[i];
            decision.track_generations[i] =
                available.decision.track_generations[i];
        }
    }
    filter_present_decision_against_tracks(decision, tracks_);

    result.has_frame = present_decision_has_frame(decision);
    if (result.has_frame) {
        result.decision = decision;
        result.first_pts_us = first_present_decision_frame_pts_us(decision);
    }
    return result;
}

RendererPausedLayoutDecision RendererTrackController::paused_layout_decision(
    const PresentDecision& last_decision) const {
    RendererPausedLayoutDecision result;
    result.active_track_count = active_track_count(tracks_);

    const auto cached = paused_cached_decision(last_decision);
    if (!cached.has_frame) {
        return result;
    }

    result.decision = cached.decision;
    result.has_frame = true;
    if (result.active_track_count > 1 &&
        !present_decision_covers_active_tracks(result.decision, tracks_)) {
        const auto preview = paused_preview_snapshot();
        if (preview.ready_to_present) {
            result.decision = preview.decision;
        } else {
            result.has_frame = false;
        }
    }
    return result;
}

RendererPausedRefreshDecision RendererTrackController::paused_refresh_decision(
    const PresentDecision& last_decision,
    const std::optional<PresentDecision>& evaluated_decision,
    bool decoded_preview_refresh) const {
    RendererPausedRefreshDecision result;
    PresentDecision decision;

    if (decoded_preview_refresh) {
        const auto snapshot = paused_preview_snapshot();
        if (snapshot.ready_to_present) {
            decision = snapshot.decision;
            result.has_frame = true;
        }
    } else if (evaluated_decision.has_value()) {
        decision = *evaluated_decision;
        filter_present_decision_against_tracks(decision, tracks_);
        if (decision.should_present) {
            apply_present_carry_forward(tracks_, last_decision, decision);
        }
        result.has_frame = present_decision_has_frame(decision);
    }

    if (!decoded_preview_refresh &&
        !result.has_frame &&
        present_decision_has_frame(last_decision)) {
        decision = last_decision;
        result.has_frame = true;
    }

    if (result.has_frame) {
        const auto available = build_available_paused_frame_snapshot(tracks_);
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!decision.frames[i].has_value() &&
                available.decision.frames[i].has_value()) {
                decision.frames[i] = available.decision.frames[i];
                decision.file_ids[i] = available.decision.file_ids[i];
                decision.track_generations[i] =
                    available.decision.track_generations[i];
            }
        }
        filter_present_decision_against_tracks(decision, tracks_);
        result.has_frame = present_decision_has_frame(decision);
    }

    if (result.has_frame &&
        active_track_count(tracks_) > 1 &&
        !present_decision_covers_active_tracks(decision, tracks_)) {
        PresentDecision cached = last_decision;
        filter_present_decision_against_tracks(cached, tracks_);
        if (!decoded_preview_refresh &&
            present_decision_covers_active_tracks(cached, tracks_)) {
            decision = cached;
        } else {
            const auto snapshot = paused_preview_snapshot();
            if (snapshot.ready_to_present) {
                decision = snapshot.decision;
                result.has_frame = true;
            } else {
                result.has_frame = false;
            }
        }
    }

    if (result.has_frame) {
        result.decision = decision;
    }
    return result;
}

bool RendererTrackController::has_complete_cached_decision(
    const PresentDecision& last_decision) const {
    PresentDecision cached = last_decision;
    filter_present_decision_against_tracks(cached, tracks_);
    const auto active_count = active_track_count(tracks_);
    return present_decision_has_frame(cached) &&
           (active_count <= 1 ||
            present_decision_covers_active_tracks(cached, tracks_));
}

PausedPreviewSnapshot RendererTrackController::paused_preview_snapshot() const {
    return build_paused_preview_snapshot(tracks_);
}

RendererTrackReferenceSnapshot
RendererTrackController::first_active_reference() const {
    RendererTrackReferenceSnapshot result;
    result.slot = first_active_slot();
    if (result.slot >= 0 && tracks_[static_cast<size_t>(result.slot)]) {
        result.offset_us = tracks_[static_cast<size_t>(result.slot)]->offset_us;
    }
    return result;
}

int RendererTrackController::next_file_id() const {
    return next_file_id_;
}

void RendererTrackController::set_next_file_id(int value) {
    next_file_id_ = value;
}

int RendererTrackController::allocate_next_file_id() {
    return next_file_id_++;
}

uint64_t RendererTrackController::allocate_generation() {
    return next_generation_++;
}

void RendererTrackController::reset_ids() {
    next_file_id_ = 1;
    next_generation_ = 1;
}

InitialTrackOpenResult RendererTrackController::open_initial_tracks(
    const std::vector<std::string>& video_paths,
    bool use_hardware_decode,
    const InitialTrackOpenHooks& hooks,
    const char* log_context) {
    return open_initial_track_pipelines(
        tracks_, video_paths, use_hardware_decode, hooks, log_context);
}

void RendererTrackController::bind_to_render_sink(RenderSink& render_sink) const {
    bind_existing_tracks_to_render_sink(tracks_, render_sink);
}

void RendererTrackController::stop_all(
    const TrackPipelineManager::TrackCallback& before_stop) {
    tracks_.stop_all(before_stop);
}

RendererTrackAddReservation RendererTrackController::reserve_add_track(
    int requested_file_id) {
    RendererTrackAddReservation reservation;
    const int slot = find_empty_slot();
    if (slot < 0) {
        reservation.failure = RendererTrackAddReservationFailure::NoEmptySlot;
        return reservation;
    }
    if (requested_file_id >= 0 && find_slot_by_file_id(requested_file_id) >= 0) {
        reservation.failure = RendererTrackAddReservationFailure::DuplicateFileId;
        return reservation;
    }

    reservation.ok = true;
    reservation.slot = slot;
    if (requested_file_id >= 0) {
        reservation.file_id = requested_file_id;
        set_next_file_id(std::max(next_file_id_, requested_file_id + 1));
    } else {
        reservation.file_id = allocate_next_file_id();
    }
    reservation.generation = allocate_generation();
    return reservation;
}

bool RendererTrackController::can_commit_add(size_t slot) const {
    return slot < kMaxTracks && !tracks_[slot];
}

TrackPipeline* RendererTrackController::commit_new_track(
    size_t slot,
    std::unique_ptr<TrackPipeline> pipeline,
    const TrackAddCommitHooks& hooks) {
    return commit_new_track_pipeline(tracks_, slot, std::move(pipeline), hooks);
}

RendererTrackDetachResult RendererTrackController::detach_and_compact_by_file_id(
    int file_id,
    const RendererTrackDetachHooks& hooks) {
    RendererTrackDetachResult result;
    const int slot = find_slot_by_file_id(file_id);
    if (slot < 0) {
        return result;
    }
    result.removed = true;
    result.slot = slot;
    const auto detached_slot = static_cast<size_t>(slot);
    if (hooks.clear_slot && tracks_[detached_slot]) {
        hooks.clear_slot(detached_slot, *tracks_[detached_slot]);
    }
    result.detached_track = std::move(tracks_[detached_slot]);
    if (result.detached_track && result.detached_track->demux_thread) {
        result.detached_track->demux_thread->set_seek_callback({});
        result.detached_track->demux_thread->set_error_callback({});
    }

    tracks_.compact_from(detached_slot, [&](size_t from, size_t to, TrackPipeline& track) {
        if (hooks.move_slot) {
            hooks.move_slot(from, to, track);
        }
    });
    recompute_cached_duration();
    result.remaining = count();
    return result;
}

RendererTrackRecreateDetachResult RendererTrackController::detach_for_recreate(
    size_t slot,
    const RendererTrackRecreateDetachHooks& hooks) {
    RendererTrackRecreateDetachResult result;
    if (slot >= kMaxTracks || !tracks_[slot]) {
        return result;
    }
    auto& current = tracks_[slot];
    result.detached = true;
    result.slot = slot;
    result.file_path = current->file_path;
    result.file_id = current->file_id;
    result.offset_us = current->offset_us;
    result.use_hardware_decode = current->use_hardware_decode;
    result.replacement_generation = allocate_generation();
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

bool RendererTrackController::can_commit_recreated_track(size_t slot) const {
    return can_commit_add(slot);
}

TrackPipeline* RendererTrackController::commit_recreated_track(
    size_t slot,
    std::unique_ptr<TrackPipeline> pipeline,
    const TrackAddCommitHooks& hooks) {
    return commit_new_track(slot, std::move(pipeline), hooks);
}

std::vector<RendererTrackSeekApplicationResult>
RendererTrackController::apply_seek_to_all(
    int64_t target_pts_us,
    SeekType type,
    bool playing,
    bool force_recreate_paused_hevc,
    const RendererTrackSeekHooks& hooks) {
    std::vector<RendererTrackSeekApplicationResult> results;
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
            tracks_, i, target_pts_us, type, playing,
            force_recreate_paused_hevc, seek_hooks);
        if (!seek_result.slot_present) {
            continue;
        }

        RendererTrackSeekApplicationResult result;
        result.slot = i;
        result.seek = std::move(seek_result);
        if (result.seek.execution.applied_seek &&
            tracks_[i] &&
            tracks_[i]->track_buffer) {
            result.buffered_frames_after = tracks_[i]->track_buffer->total_count();
        }
        results.push_back(std::move(result));
    }
    return results;
}

bool RendererTrackController::apply_seek_to_all_and_log(
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

int64_t RendererTrackController::cached_duration_us() const {
    return cached_duration_us_;
}

void RendererTrackController::set_cached_duration_us(int64_t duration_us) {
    cached_duration_us_ = duration_us;
}

void RendererTrackController::extend_cached_duration_with(
    const TrackPipeline& track) {
    cached_duration_us_ = extend_track_duration_cache(cached_duration_us_, track);
}

void RendererTrackController::recompute_cached_duration() {
    cached_duration_us_ = compute_track_duration_cache(tracks_);
}

int64_t RendererTrackController::effective_duration_us() const {
    return resolve_effective_duration_us(tracks_, cached_duration_us_);
}

int64_t RendererTrackController::offset_us_for_file_id(int file_id) const {
    const int slot = find_slot_by_file_id(file_id);
    if (slot < 0 || !tracks_[static_cast<size_t>(slot)]) {
        return 0;
    }
    return tracks_[static_cast<size_t>(slot)]->offset_us;
}

TrackAddSeekResult RendererTrackController::prepare_add_track_seek_to_clock(
    TrackPipeline& track,
    int64_t current_pts_us,
    bool was_playing,
    const TrackAddSeekHooks& hooks) const {
    return vr::prepare_add_track_seek_to_clock(
        track, current_pts_us, was_playing, hooks);
}

TrackPerfBaselineTracker& RendererTrackController::perf_baseline_tracker() {
    return perf_baseline_tracker_;
}

const TrackPerfBaselineTracker& RendererTrackController::perf_baseline_tracker() const {
    return perf_baseline_tracker_;
}

} // namespace vr
