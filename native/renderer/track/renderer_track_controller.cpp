#include "renderer/track/renderer_track_controller.h"

#include "renderer/seek/renderer_seek_log_policy.h"
#include "renderer/track/track_lifecycle.h"
#include "renderer/track/track_preroll_policy.h"

#include <algorithm>
#include <memory>

namespace vr {

class RendererTrackRegistry {
public:
    std::unique_ptr<TrackPipeline> create_pipeline(
        const std::string& path,
        bool hw_decode,
        RenderBackendKind render_backend,
        const SeekRequest* initial_seek) const {
        TrackPipelineOpenOptions options;
        options.render_backend = render_backend;
        return factory.create_opened_pipeline(
            path, hw_decode, initial_seek, options);
    }

    void assign_missing_generations() {
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (tracks[i] && tracks[i]->generation == 0) {
                tracks[i]->generation = allocate_generation();
            }
        }
    }

    int first_active_slot() const { return tracks.first_active_slot(); }
    int find_empty_slot() const { return tracks.find_empty_slot(); }
    int find_slot_by_file_id(int file_id) const {
        return tracks.find_slot_by_file_id(file_id);
    }

    int audio_info_slot(int preferred_file_id) const {
        if (preferred_file_id >= 0) {
            const int slot = find_slot_by_file_id(preferred_file_id);
            if (slot >= 0 && tracks[static_cast<size_t>(slot)] &&
                tracks[static_cast<size_t>(slot)]->demux_thread) {
                const auto& stats =
                    tracks[static_cast<size_t>(slot)]->demux_thread->stats();
                if (stats.audio_stream_index >= 0 && stats.audio_codec_params) {
                    return slot;
                }
            }
        }

        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!tracks[i] || !tracks[i]->demux_thread) {
                continue;
            }
            const auto& stats = tracks[i]->demux_thread->stats();
            if (stats.audio_stream_index >= 0 && stats.audio_codec_params) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int audio_sample_rate_for_slot(int slot) const {
        if (slot < 0 || slot >= static_cast<int>(kMaxTracks) ||
            !tracks[static_cast<size_t>(slot)] ||
            !tracks[static_cast<size_t>(slot)]->demux_thread) {
            return 0;
        }
        return tracks[static_cast<size_t>(slot)]->demux_thread->stats().sample_rate;
    }

    int audio_channels_for_slot(int slot) const {
        if (slot < 0 || slot >= static_cast<int>(kMaxTracks) ||
            !tracks[static_cast<size_t>(slot)] ||
            !tracks[static_cast<size_t>(slot)]->demux_thread) {
            return 0;
        }
        return tracks[static_cast<size_t>(slot)]->demux_thread->stats().channels;
    }

    bool uses_hardware_codec(AVCodecID codec_id) const {
        return any_track_uses_hardware_codec(tracks, codec_id);
    }

    int64_t min_current_frame_duration_us() const {
        return compute_min_current_frame_duration_us(tracks);
    }

    bool has_slot(int slot) const {
        return slot >= 0 &&
               slot < static_cast<int>(kMaxTracks) &&
               tracks[static_cast<size_t>(slot)] != nullptr;
    }

    std::pair<int, int> dimensions_for_slot(int slot) const {
        if (!has_slot(slot)) {
            return {0, 0};
        }
        const auto& track = tracks[static_cast<size_t>(slot)];
        return {track->video_width, track->video_height};
    }

    bool has_active_tracks() const { return tracks.has_active_tracks(); }
    size_t count() const { return tracks.count(); }

    bool has_preroll_blocking_track() const {
        return vr::has_preroll_blocking_track(tracks);
    }

    bool has_buffering_track() const {
        return vr::has_buffering_track(tracks);
    }

    int next_file_id_value() const { return next_file_id; }
    void set_next_file_id(int value) { next_file_id = value; }
    int allocate_next_file_id() { return next_file_id++; }
    uint64_t allocate_generation() { return next_generation++; }

    void reset_ids() {
        next_file_id = 1;
        next_generation = 1;
    }

    InitialTrackOpenResult open_initial_tracks(
        const std::vector<std::string>& video_paths,
        bool use_hardware_decode,
        const InitialTrackOpenHooks& hooks,
        const char* log_context) {
        return open_initial_track_pipelines(
            tracks, video_paths, use_hardware_decode, hooks, log_context);
    }

    void bind_to_render_sink(RenderSink& render_sink) const {
        bind_existing_tracks_to_render_sink(tracks, render_sink);
    }

    void stop_all(const RendererTrackBeforeStopCallback& before_stop = {}) {
        tracks.stop_all(before_stop);
    }

    RendererTrackAddReservation reserve_add_track(int requested_file_id) {
        RendererTrackAddReservation reservation;
        const int slot = find_empty_slot();
        if (slot < 0) {
            reservation.failure = RendererTrackAddReservationFailure::NoEmptySlot;
            return reservation;
        }
        if (requested_file_id >= 0 &&
            find_slot_by_file_id(requested_file_id) >= 0) {
            reservation.failure =
                RendererTrackAddReservationFailure::DuplicateFileId;
            return reservation;
        }

        reservation.ok = true;
        reservation.slot = slot;
        if (requested_file_id >= 0) {
            reservation.file_id = requested_file_id;
            set_next_file_id(std::max(next_file_id, requested_file_id + 1));
        } else {
            reservation.file_id = allocate_next_file_id();
        }
        reservation.generation = allocate_generation();
        return reservation;
    }

    int64_t cached_duration() const { return cached_duration_us; }
    void set_cached_duration(int64_t duration_us) {
        cached_duration_us = duration_us;
    }
    void extend_cached_duration_with(const TrackPipeline& track) {
        cached_duration_us = extend_track_duration_cache(cached_duration_us, track);
    }
    void recompute_cached_duration() {
        cached_duration_us = compute_track_duration_cache(tracks);
    }
    int64_t effective_duration_us() const {
        return resolve_effective_duration_us(tracks, cached_duration_us);
    }
    int64_t offset_us_for_file_id(int file_id) const {
        const int slot = find_slot_by_file_id(file_id);
        if (slot < 0 || !tracks[static_cast<size_t>(slot)]) {
            return 0;
        }
        return tracks[static_cast<size_t>(slot)]->offset_us;
    }

    TrackAddSeekResult prepare_add_track_seek_to_clock(
        TrackPipeline& track,
        int64_t current_pts_us,
        bool was_playing,
        const TrackAddSeekHooks& hooks) const {
        return vr::prepare_add_track_seek_to_clock(
            track, current_pts_us, was_playing, hooks);
    }

    TrackPipelineFactory factory;
    TrackPipelineManager tracks;
    int next_file_id = 1;
    uint64_t next_generation = 1;
    int64_t cached_duration_us = 0;
};

class RendererTrackMutationController {
public:
    explicit RendererTrackMutationController(RendererTrackRegistry& registry)
        : registry_(registry) {}

    bool configure_and_start_pipeline(TrackPipeline& pipeline,
                                      const TrackPipelineStartConfig& config,
                                      const TrackPipelineStartHooks& hooks,
                                      const char* log_context) const {
        return configure_and_start_track_pipeline(
            pipeline, config, hooks, log_context);
    }

    void stop_detached_pipeline(
        size_t slot,
        std::unique_ptr<TrackPipeline>& track) const {
        stop_detached_track_pipeline(slot, track);
    }

    void set_video_decode_paused(
        bool paused,
        const std::function<void(size_t slot, TrackPipeline& track, bool paused)>&
            set_decode_paused) {
        apply_track_video_decode_pause_state(
            registry_.tracks, paused, set_decode_paused);
    }

    void set_decode_paused_for_all(bool paused,
                                   const TrackDecodePauseHooks& hooks) {
        apply_track_decode_pause_state(registry_.tracks, paused, hooks);
    }

    void apply_playback_decode_state(
        bool playback_active,
        const TrackPlaybackDecodeStateHooks& hooks) {
        vr::apply_track_playback_decode_state(
            registry_.tracks, playback_active, hooks);
    }

    bool apply_track_offset(int file_id,
                            int64_t offset_us,
                            const TrackOffsetMutationHooks& hooks) {
        const int slot = registry_.find_slot_by_file_id(file_id);
        if (slot < 0 || !registry_.tracks[static_cast<size_t>(slot)]) {
            return false;
        }
        apply_track_offset_mutation(
            *registry_.tracks[static_cast<size_t>(slot)],
            static_cast<size_t>(slot),
            offset_us,
            hooks);
        return true;
    }

    StepDecisionApplication apply_step_forward_decision(
        int64_t current_pts_us,
        const PresentDecision& decision,
        const PresentDecision& last_decision) {
        return vr::apply_step_forward_decision(
            registry_.tracks, current_pts_us, decision, last_decision);
    }

    void discard_step_forward_consumed_frames(
        int64_t current_pts_us,
        const PresentDecision& decision,
        const PresentDecision& last_decision) {
        vr::discard_step_forward_consumed_frames(
            registry_.tracks, current_pts_us, decision, last_decision);
    }

    StepDecisionApplication apply_step_backward_decision(
        const PresentDecision& decision) {
        return vr::apply_step_backward_decision(registry_.tracks, decision);
    }

    bool can_commit_add(size_t slot) const {
        return slot < kMaxTracks && !registry_.tracks[slot];
    }

    TrackPipeline* commit_new_track(
        size_t slot,
        std::unique_ptr<TrackPipeline> pipeline,
        const TrackAddCommitHooks& hooks) {
        return commit_new_track_pipeline(
            registry_.tracks, slot, std::move(pipeline), hooks);
    }

    RendererTrackDetachResult detach_and_compact_by_file_id(
        int file_id,
        const RendererTrackDetachHooks& hooks) {
        RendererTrackDetachResult result;
        const int slot = registry_.find_slot_by_file_id(file_id);
        if (slot < 0) {
            return result;
        }
        result.removed = true;
        result.slot = slot;
        const auto detached_slot = static_cast<size_t>(slot);
        if (hooks.clear_slot && registry_.tracks[detached_slot]) {
            hooks.clear_slot(detached_slot, *registry_.tracks[detached_slot]);
        }
        result.detached_track = std::move(registry_.tracks[detached_slot]);
        if (result.detached_track && result.detached_track->demux_thread) {
            result.detached_track->demux_thread->set_seek_callback({});
            result.detached_track->demux_thread->set_error_callback({});
        }

        registry_.tracks.compact_from(
            detached_slot, [&](size_t from, size_t to, TrackPipeline& track) {
                if (hooks.move_slot) {
                    hooks.move_slot(from, to, track);
                }
            });
        registry_.recompute_cached_duration();
        result.remaining = registry_.count();
        return result;
    }

    RendererTrackRecreateDetachResult detach_for_recreate(
        size_t slot,
        const RendererTrackRecreateDetachHooks& hooks) {
        RendererTrackRecreateDetachResult result;
        if (slot >= kMaxTracks || !registry_.tracks[slot]) {
            return result;
        }
        auto& current = registry_.tracks[slot];
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

    bool can_commit_recreated_track(size_t slot) const {
        return can_commit_add(slot);
    }

    TrackPipeline* commit_recreated_track(
        size_t slot,
        std::unique_ptr<TrackPipeline> pipeline,
        const TrackAddCommitHooks& hooks) {
        return commit_new_track(slot, std::move(pipeline), hooks);
    }

    std::vector<RendererTrackSeekApplicationResult> apply_seek_to_all(
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
                registry_.tracks, i, target_pts_us, type, playing,
                force_recreate_paused_hevc, seek_hooks);
            if (!seek_result.slot_present) {
                continue;
            }

            RendererTrackSeekApplicationResult result;
            result.slot = i;
            result.seek = std::move(seek_result);
            if (result.seek.execution.applied_seek &&
                registry_.tracks[i] &&
                registry_.tracks[i]->track_buffer) {
                result.buffered_frames_after =
                    registry_.tracks[i]->track_buffer->total_count();
            }
            results.push_back(std::move(result));
        }
        return results;
    }

    bool apply_seek_to_all_and_log(
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

private:
    RendererTrackRegistry& registry_;
};

class RendererTrackPresentationModel {
public:
    explicit RendererTrackPresentationModel(RendererTrackRegistry& registry)
        : registry_(registry) {}

    std::vector<TrackInfo> infos() const {
        return snapshot_track_infos(registry_.tracks);
    }

    std::vector<TrackPerfStats> perf_stats(
        const PresentDecision& last_decision,
        std::chrono::steady_clock::time_point now) {
        const double elapsed_s = perf_baseline_tracker.elapsed_seconds(now);
        const bool should_rotate =
            perf_baseline_tracker.should_rotate(elapsed_s);
        PresentDecision filtered_last_decision = last_decision;
        filter_present_decision_against_tracks(
            filtered_last_decision, registry_.tracks);
        const auto snapshot = snapshot_track_perf_stats_collection(
            registry_.tracks,
            filtered_last_decision,
            perf_baseline_tracker,
            elapsed_s);

        if (should_rotate) {
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!snapshot.frames_decoded_by_slot[i].has_value()) {
                    continue;
                }
                perf_baseline_tracker.update_baseline_frames(
                    i, *snapshot.frames_decoded_by_slot[i]);
            }
            perf_baseline_tracker.rotate_timer(now);
        }
        return snapshot.stats;
    }

    TrackGpuMemoryStatsCollectionResult gpu_memory_stats(
        const std::array<uint64_t, kMaxTracks>&
            presenter_copy_texture_bytes_by_slot) const {
        return snapshot_track_gpu_memory_stats_collection(
            registry_.tracks, presenter_copy_texture_bytes_by_slot);
    }

    LayoutTrackGeometryList layout_track_geometry() const {
        return snapshot_layout_track_geometry(registry_.tracks);
    }

    void populate_draw_tracks(RendererDrawTrackSnapshotList& out) const {
        out = {};
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!registry_.tracks[i]) {
                continue;
            }
            auto& track = out[i];
            track.active = true;
            track.file_id = registry_.tracks[i]->file_id;
            track.generation = registry_.tracks[i]->generation;
            track.offset_us = registry_.tracks[i]->offset_us;
            track.video_width = registry_.tracks[i]->video_width;
            track.video_height = registry_.tracks[i]->video_height;
            track.video_aspect = registry_.tracks[i]->video_aspect;
        }
    }

    std::vector<RendererLayoutTrackReference> layout_track_references() const {
        std::vector<RendererLayoutTrackReference> refs;
        refs.reserve(registry_.count());
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!registry_.tracks[i]) {
                continue;
            }
            refs.push_back(RendererLayoutTrackReference{
                registry_.tracks[i]->file_id,
                static_cast<int>(i),
            });
        }
        return refs;
    }

    std::vector<RenderLoopTrackDiagnosticSnapshot>
    render_loop_diagnostics() const {
        return snapshot_render_loop_track_diagnostics(registry_.tracks);
    }

    StepDecisionBuildResult build_step_forward_decision(
        int64_t current_pts_us,
        const PresentDecision& last_decision,
        PresentDecision& decision) const {
        return build_step_forward_decision_result(
            registry_.tracks,
            current_pts_us,
            registry_.min_current_frame_duration_us(),
            last_decision,
            decision);
    }

    StepForwardExactSeekTarget choose_step_forward_exact_seek_target(
        int64_t clock_pts_us,
        const PresentDecision& last_decision) const {
        return vr::choose_step_forward_exact_seek_target(
            registry_.tracks,
            clock_pts_us,
            registry_.cached_duration_us,
            last_decision);
    }

    bool build_step_backward_decision(
        int64_t current_pts_us,
        const PresentDecision& last_decision,
        PresentDecision& decision) const {
        return vr::build_step_backward_decision(
            registry_.tracks, current_pts_us, last_decision, decision);
    }

    StepBackwardExactSeekTarget choose_step_backward_exact_seek_target(
        int64_t clock_pts_us,
        const PresentDecision& last_decision) const {
        return vr::choose_step_backward_exact_seek_target(
            registry_.tracks, clock_pts_us, last_decision);
    }

    void apply_carry_forward(const PresentDecision& last_decision,
                             PresentDecision& decision) const {
        apply_present_carry_forward(registry_.tracks, last_decision, decision);
    }

    void filter_present_decision(PresentDecision& decision) const {
        filter_present_decision_against_tracks(decision, registry_.tracks);
    }

    std::vector<SeekPreviewPresentedTrackEvent>
    collect_seek_preview_presented_events(
        const PresentDecision& decision,
        int64_t request_id,
        int64_t target_pts_us) const {
        return collect_seek_preview_presented_track_events(
            registry_.tracks, decision, request_id, target_pts_us);
    }

    std::vector<LayoutTrackGeometryUpdate>
    update_layout_track_geometry_from_decision(const PresentDecision& decision) {
        return vr::update_layout_track_geometry_from_decision(
            registry_.tracks, decision);
    }

    EmptyBufferEofClamp empty_buffer_eof_clamp(
        const PresentDecision& last_decision) const {
        return compute_empty_buffer_eof_clamp(registry_.tracks, last_decision);
    }

    std::optional<int64_t> next_frame_event_pts_us(
        int64_t current_pts_us) const {
        return compute_next_frame_event_pts_us(registry_.tracks, current_pts_us);
    }

    RendererPausedCachedDecision paused_cached_decision(
        const PresentDecision& last_decision) const {
        RendererPausedCachedDecision result;
        PresentDecision decision = last_decision;
        filter_present_decision_against_tracks(decision, registry_.tracks);
        if (!present_decision_has_frame(decision)) {
            return result;
        }

        const auto available =
            build_available_paused_frame_snapshot(registry_.tracks);
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!decision.frames[i].has_value() &&
                available.decision.frames[i].has_value()) {
                decision.frames[i] = available.decision.frames[i];
                decision.file_ids[i] = available.decision.file_ids[i];
                decision.track_generations[i] =
                    available.decision.track_generations[i];
            }
        }
        filter_present_decision_against_tracks(decision, registry_.tracks);

        result.has_frame = present_decision_has_frame(decision);
        if (result.has_frame) {
            result.decision = decision;
            result.first_pts_us = first_present_decision_frame_pts_us(decision);
        }
        return result;
    }

    RendererPausedLayoutDecision paused_layout_decision(
        const PresentDecision& last_decision) const {
        RendererPausedLayoutDecision result;
        result.active_track_count = active_track_count(registry_.tracks);

        const auto cached = paused_cached_decision(last_decision);
        if (!cached.has_frame) {
            return result;
        }

        result.decision = cached.decision;
        result.has_frame = true;
        if (result.active_track_count > 1 &&
            !present_decision_covers_active_tracks(
                result.decision, registry_.tracks)) {
            const auto preview = paused_preview_snapshot();
            if (preview.ready_to_present) {
                result.decision = preview.decision;
            } else {
                result.has_frame = false;
            }
        }
        return result;
    }

    RendererPausedRefreshDecision paused_refresh_decision(
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
            filter_present_decision_against_tracks(decision, registry_.tracks);
            if (decision.should_present) {
                apply_present_carry_forward(
                    registry_.tracks, last_decision, decision);
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
            const auto available =
                build_available_paused_frame_snapshot(registry_.tracks);
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!decision.frames[i].has_value() &&
                    available.decision.frames[i].has_value()) {
                    decision.frames[i] = available.decision.frames[i];
                    decision.file_ids[i] = available.decision.file_ids[i];
                    decision.track_generations[i] =
                        available.decision.track_generations[i];
                }
            }
            filter_present_decision_against_tracks(decision, registry_.tracks);
            result.has_frame = present_decision_has_frame(decision);
        }

        if (result.has_frame &&
            active_track_count(registry_.tracks) > 1 &&
            !present_decision_covers_active_tracks(decision, registry_.tracks)) {
            PresentDecision cached = last_decision;
            filter_present_decision_against_tracks(cached, registry_.tracks);
            if (!decoded_preview_refresh &&
                present_decision_covers_active_tracks(
                    cached, registry_.tracks)) {
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

    bool has_complete_cached_decision(
        const PresentDecision& last_decision) const {
        PresentDecision cached = last_decision;
        filter_present_decision_against_tracks(cached, registry_.tracks);
        const auto active_count = active_track_count(registry_.tracks);
        return present_decision_has_frame(cached) &&
               (active_count <= 1 ||
                present_decision_covers_active_tracks(cached, registry_.tracks));
    }

    PausedPreviewSnapshot paused_preview_snapshot() const {
        return build_paused_preview_snapshot(registry_.tracks);
    }

    RendererTrackReferenceSnapshot first_active_reference() const {
        RendererTrackReferenceSnapshot result;
        result.slot = registry_.first_active_slot();
        if (result.slot >= 0 &&
            registry_.tracks[static_cast<size_t>(result.slot)]) {
            result.offset_us =
                registry_.tracks[static_cast<size_t>(result.slot)]->offset_us;
        }
        return result;
    }

    TrackPerfBaselineTracker perf_baseline_tracker;

private:
    RendererTrackRegistry& registry_;
};

RendererTrackController::RendererTrackController()
    : registry_(std::make_unique<RendererTrackRegistry>()),
      mutation_(std::make_unique<RendererTrackMutationController>(*registry_)),
      presentation_model_(
          std::make_unique<RendererTrackPresentationModel>(*registry_)) {}

RendererTrackController::~RendererTrackController() = default;

std::unique_ptr<TrackPipeline> RendererTrackController::create_pipeline(
    const std::string& path,
    bool hw_decode,
    RenderBackendKind render_backend,
    const SeekRequest* initial_seek) const {
    return registry_->create_pipeline(
        path, hw_decode, render_backend, initial_seek);
}

bool RendererTrackController::configure_and_start_pipeline(
    TrackPipeline& pipeline,
    const TrackPipelineStartConfig& config,
    const TrackPipelineStartHooks& hooks,
    const char* log_context) const {
    return mutation_->configure_and_start_pipeline(
        pipeline, config, hooks, log_context);
}

void RendererTrackController::stop_detached_pipeline(
    size_t slot,
    std::unique_ptr<TrackPipeline>& track) const {
    mutation_->stop_detached_pipeline(slot, track);
}

void RendererTrackController::assign_missing_generations() {
    registry_->assign_missing_generations();
}

int RendererTrackController::first_active_slot() const {
    return registry_->first_active_slot();
}

int RendererTrackController::find_empty_slot() const {
    return registry_->find_empty_slot();
}

int RendererTrackController::find_slot_by_file_id(int file_id) const {
    return registry_->find_slot_by_file_id(file_id);
}

int RendererTrackController::audio_info_slot(int preferred_file_id) const {
    return registry_->audio_info_slot(preferred_file_id);
}

int RendererTrackController::audio_sample_rate_for_slot(int slot) const {
    return registry_->audio_sample_rate_for_slot(slot);
}

int RendererTrackController::audio_channels_for_slot(int slot) const {
    return registry_->audio_channels_for_slot(slot);
}

bool RendererTrackController::uses_hardware_codec(AVCodecID codec_id) const {
    return registry_->uses_hardware_codec(codec_id);
}

int64_t RendererTrackController::min_current_frame_duration_us() const {
    return registry_->min_current_frame_duration_us();
}

bool RendererTrackController::has_slot(int slot) const {
    return registry_->has_slot(slot);
}

std::pair<int, int> RendererTrackController::dimensions_for_slot(int slot) const {
    return registry_->dimensions_for_slot(slot);
}

std::vector<TrackInfo> RendererTrackController::infos() const {
    return presentation_model_->infos();
}

std::vector<TrackPerfStats> RendererTrackController::perf_stats(
    const PresentDecision& last_decision,
    std::chrono::steady_clock::time_point now) {
    return presentation_model_->perf_stats(last_decision, now);
}

TrackGpuMemoryStatsCollectionResult RendererTrackController::gpu_memory_stats(
    const std::array<uint64_t, kMaxTracks>& presenter_copy_texture_bytes_by_slot) const {
    return presentation_model_->gpu_memory_stats(
        presenter_copy_texture_bytes_by_slot);
}

LayoutTrackGeometryList RendererTrackController::layout_track_geometry() const {
    return presentation_model_->layout_track_geometry();
}

void RendererTrackController::populate_draw_tracks(
    RendererDrawTrackSnapshotList& out) const {
    presentation_model_->populate_draw_tracks(out);
}

std::vector<RendererLayoutTrackReference>
RendererTrackController::layout_track_references() const {
    return presentation_model_->layout_track_references();
}

bool RendererTrackController::has_active_tracks() const {
    return registry_->has_active_tracks();
}

size_t RendererTrackController::count() const {
    return registry_->count();
}

bool RendererTrackController::has_preroll_blocking_track() const {
    return registry_->has_preroll_blocking_track();
}

bool RendererTrackController::has_buffering_track() const {
    return registry_->has_buffering_track();
}

std::vector<RenderLoopTrackDiagnosticSnapshot>
RendererTrackController::render_loop_diagnostics() const {
    return presentation_model_->render_loop_diagnostics();
}

void RendererTrackController::set_video_decode_paused(
    bool paused,
    const std::function<void(size_t slot, TrackPipeline& track, bool paused)>&
        set_decode_paused) {
    mutation_->set_video_decode_paused(paused, set_decode_paused);
}

void RendererTrackController::set_decode_paused_for_all(
    bool paused,
    const TrackDecodePauseHooks& hooks) {
    mutation_->set_decode_paused_for_all(paused, hooks);
}

void RendererTrackController::apply_playback_decode_state(
    bool playback_active,
    const TrackPlaybackDecodeStateHooks& hooks) {
    mutation_->apply_playback_decode_state(playback_active, hooks);
}

bool RendererTrackController::apply_track_offset(
    int file_id,
    int64_t offset_us,
    const TrackOffsetMutationHooks& hooks) {
    return mutation_->apply_track_offset(file_id, offset_us, hooks);
}

StepDecisionBuildResult RendererTrackController::build_step_forward_decision(
    int64_t current_pts_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    return presentation_model_->build_step_forward_decision(
        current_pts_us, last_decision, decision);
}

StepDecisionApplication RendererTrackController::apply_step_forward_decision(
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision) {
    return mutation_->apply_step_forward_decision(
        current_pts_us, decision, last_decision);
}

void RendererTrackController::discard_step_forward_consumed_frames(
    int64_t current_pts_us,
    const PresentDecision& decision,
    const PresentDecision& last_decision) {
    mutation_->discard_step_forward_consumed_frames(
        current_pts_us, decision, last_decision);
}

StepForwardExactSeekTarget
RendererTrackController::choose_step_forward_exact_seek_target(
    int64_t clock_pts_us,
    const PresentDecision& last_decision) const {
    return presentation_model_->choose_step_forward_exact_seek_target(
        clock_pts_us, last_decision);
}

bool RendererTrackController::build_step_backward_decision(
    int64_t current_pts_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    return presentation_model_->build_step_backward_decision(
        current_pts_us, last_decision, decision);
}

StepDecisionApplication RendererTrackController::apply_step_backward_decision(
    const PresentDecision& decision) {
    return mutation_->apply_step_backward_decision(decision);
}

StepBackwardExactSeekTarget
RendererTrackController::choose_step_backward_exact_seek_target(
    int64_t clock_pts_us,
    const PresentDecision& last_decision) const {
    return presentation_model_->choose_step_backward_exact_seek_target(
        clock_pts_us, last_decision);
}

void RendererTrackController::apply_carry_forward(
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    presentation_model_->apply_carry_forward(last_decision, decision);
}

void RendererTrackController::filter_present_decision(PresentDecision& decision) const {
    presentation_model_->filter_present_decision(decision);
}

std::vector<SeekPreviewPresentedTrackEvent>
RendererTrackController::collect_seek_preview_presented_events(
    const PresentDecision& decision,
    int64_t request_id,
    int64_t target_pts_us) const {
    return presentation_model_->collect_seek_preview_presented_events(
        decision, request_id, target_pts_us);
}

std::vector<LayoutTrackGeometryUpdate>
RendererTrackController::update_layout_track_geometry_from_decision(
    const PresentDecision& decision) {
    return presentation_model_->update_layout_track_geometry_from_decision(
        decision);
}

EmptyBufferEofClamp RendererTrackController::empty_buffer_eof_clamp(
    const PresentDecision& last_decision) const {
    return presentation_model_->empty_buffer_eof_clamp(last_decision);
}

std::optional<int64_t> RendererTrackController::next_frame_event_pts_us(
    int64_t current_pts_us) const {
    return presentation_model_->next_frame_event_pts_us(current_pts_us);
}

RendererPausedCachedDecision RendererTrackController::paused_cached_decision(
    const PresentDecision& last_decision) const {
    return presentation_model_->paused_cached_decision(last_decision);
}

RendererPausedLayoutDecision RendererTrackController::paused_layout_decision(
    const PresentDecision& last_decision) const {
    return presentation_model_->paused_layout_decision(last_decision);
}

RendererPausedRefreshDecision RendererTrackController::paused_refresh_decision(
    const PresentDecision& last_decision,
    const std::optional<PresentDecision>& evaluated_decision,
    bool decoded_preview_refresh) const {
    return presentation_model_->paused_refresh_decision(
        last_decision, evaluated_decision, decoded_preview_refresh);
}

bool RendererTrackController::has_complete_cached_decision(
    const PresentDecision& last_decision) const {
    return presentation_model_->has_complete_cached_decision(last_decision);
}

PausedPreviewSnapshot RendererTrackController::paused_preview_snapshot() const {
    return presentation_model_->paused_preview_snapshot();
}

RendererTrackReferenceSnapshot
RendererTrackController::first_active_reference() const {
    return presentation_model_->first_active_reference();
}

int RendererTrackController::next_file_id() const {
    return registry_->next_file_id_value();
}

void RendererTrackController::set_next_file_id(int value) {
    registry_->set_next_file_id(value);
}

int RendererTrackController::allocate_next_file_id() {
    return registry_->allocate_next_file_id();
}

uint64_t RendererTrackController::allocate_generation() {
    return registry_->allocate_generation();
}

void RendererTrackController::reset_ids() {
    registry_->reset_ids();
}

InitialTrackOpenResult RendererTrackController::open_initial_tracks(
    const std::vector<std::string>& video_paths,
    bool use_hardware_decode,
    const InitialTrackOpenHooks& hooks,
    const char* log_context) {
    return registry_->open_initial_tracks(
        video_paths, use_hardware_decode, hooks, log_context);
}

void RendererTrackController::bind_to_render_sink(RenderSink& render_sink) const {
    registry_->bind_to_render_sink(render_sink);
}

void RendererTrackController::stop_all(
    const RendererTrackBeforeStopCallback& before_stop) {
    registry_->stop_all(before_stop);
}

RendererTrackAddReservation RendererTrackController::reserve_add_track(
    int requested_file_id) {
    return registry_->reserve_add_track(requested_file_id);
}

bool RendererTrackController::can_commit_add(size_t slot) const {
    return mutation_->can_commit_add(slot);
}

TrackPipeline* RendererTrackController::commit_new_track(
    size_t slot,
    std::unique_ptr<TrackPipeline> pipeline,
    const TrackAddCommitHooks& hooks) {
    return mutation_->commit_new_track(slot, std::move(pipeline), hooks);
}

RendererTrackDetachResult RendererTrackController::detach_and_compact_by_file_id(
    int file_id,
    const RendererTrackDetachHooks& hooks) {
    return mutation_->detach_and_compact_by_file_id(file_id, hooks);
}

RendererTrackRecreateDetachResult RendererTrackController::detach_for_recreate(
    size_t slot,
    const RendererTrackRecreateDetachHooks& hooks) {
    return mutation_->detach_for_recreate(slot, hooks);
}

bool RendererTrackController::can_commit_recreated_track(size_t slot) const {
    return mutation_->can_commit_recreated_track(slot);
}

TrackPipeline* RendererTrackController::commit_recreated_track(
    size_t slot,
    std::unique_ptr<TrackPipeline> pipeline,
    const TrackAddCommitHooks& hooks) {
    return mutation_->commit_recreated_track(slot, std::move(pipeline), hooks);
}

std::vector<RendererTrackSeekApplicationResult>
RendererTrackController::apply_seek_to_all(
    int64_t target_pts_us,
    SeekType type,
    bool playing,
    bool force_recreate_paused_hevc,
    const RendererTrackSeekHooks& hooks) {
    return mutation_->apply_seek_to_all(
        target_pts_us, type, playing, force_recreate_paused_hevc, hooks);
}

bool RendererTrackController::apply_seek_to_all_and_log(
    int64_t target_pts_us,
    SeekType type,
    bool playing,
    bool force_recreate_paused_hevc,
    const RendererTrackSeekHooks& hooks) {
    return mutation_->apply_seek_to_all_and_log(
        target_pts_us, type, playing, force_recreate_paused_hevc, hooks);
}

int64_t RendererTrackController::cached_duration_us() const {
    return registry_->cached_duration();
}

void RendererTrackController::set_cached_duration_us(int64_t duration_us) {
    registry_->set_cached_duration(duration_us);
}

void RendererTrackController::extend_cached_duration_with(
    const TrackPipeline& track) {
    registry_->extend_cached_duration_with(track);
}

void RendererTrackController::recompute_cached_duration() {
    registry_->recompute_cached_duration();
}

int64_t RendererTrackController::effective_duration_us() const {
    return registry_->effective_duration_us();
}

int64_t RendererTrackController::offset_us_for_file_id(int file_id) const {
    return registry_->offset_us_for_file_id(file_id);
}

TrackAddSeekResult RendererTrackController::prepare_add_track_seek_to_clock(
    TrackPipeline& track,
    int64_t current_pts_us,
    bool was_playing,
    const TrackAddSeekHooks& hooks) const {
    return registry_->prepare_add_track_seek_to_clock(
        track, current_pts_us, was_playing, hooks);
}

TrackPerfBaselineTracker& RendererTrackController::perf_baseline_tracker() {
    return presentation_model_->perf_baseline_tracker;
}

const TrackPerfBaselineTracker& RendererTrackController::perf_baseline_tracker() const {
    return presentation_model_->perf_baseline_tracker;
}

} // namespace vr
