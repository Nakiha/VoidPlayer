#pragma once

#include "renderer/track/track_pipeline_factory.h"
#include "renderer/track/track_lifecycle.h"
#include "renderer/track/track_playback_pacing.h"
#include "renderer/track/track_preview_policy.h"
#include "renderer/track/track_present_policy.h"
#include "renderer/track/track_snapshot.h"
#include "renderer/track/track_step_policy.h"
#include "renderer/track/renderer_track_types.h"
#include "renderer/render/renderer_draw_snapshot.h"

#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <vector>

namespace vr {

class RendererTrackMutationController;
class RendererTrackPresentationModel;
class RendererTrackRegistry;

// Lock contract:
// - Compatibility facade over track registry, mutation, and presentation-model
//   ownership components.
// - Does not take renderer locks or call host/platform callbacks.
// - Current callers still hold owner locks around mutation sequences;
//   future steps can move add/remove/seek orchestration behind command/result APIs.
class RendererTrackController {
public:
    RendererTrackController();
    ~RendererTrackController();

    RendererTrackController(const RendererTrackController&) = delete;
    RendererTrackController& operator=(const RendererTrackController&) = delete;

    std::unique_ptr<TrackPipeline> create_pipeline(
        const std::string& path,
        bool hw_decode,
        RenderBackendKind render_backend,
        void* render_device = nullptr,
        std::recursive_mutex* device_mutex = nullptr,
        const SeekRequest* initial_seek = nullptr) const;
    bool configure_and_start_pipeline(TrackPipeline& pipeline,
                                      const TrackPipelineStartConfig& config,
                                      const TrackPipelineStartHooks& hooks,
                                      const char* log_context) const;
    void stop_detached_pipeline(size_t slot,
                                std::unique_ptr<TrackPipeline>& track) const;

    void assign_missing_generations();
    int first_active_slot() const;
    int find_empty_slot() const;
    int find_slot_by_file_id(int file_id) const;
    int audio_info_slot(int preferred_file_id) const;
    int audio_sample_rate_for_slot(int slot) const;
    int audio_channels_for_slot(int slot) const;
    bool uses_hardware_codec(AVCodecID codec_id) const;
    int64_t min_current_frame_duration_us() const;
    bool has_slot(int slot) const;
    std::pair<int, int> dimensions_for_slot(int slot) const;
    std::vector<TrackInfo> infos() const;
    std::vector<TrackPerfStats> perf_stats(const PresentDecision& last_decision,
                                           std::chrono::steady_clock::time_point now);
    TrackGpuMemoryStatsCollectionResult gpu_memory_stats(
        const std::array<uint64_t, kMaxTracks>& presenter_copy_texture_bytes_by_slot) const;
    LayoutTrackGeometryList layout_track_geometry() const;
    void populate_draw_tracks(RendererDrawTrackSnapshotList& out) const;
    std::vector<RendererLayoutTrackReference> layout_track_references() const;
    bool has_active_tracks() const;
    size_t count() const;
    bool has_buffering_track() const;
    std::vector<RenderLoopTrackDiagnosticSnapshot> render_loop_diagnostics() const;
    PlaybackPacingSnapshot playback_pacing_snapshot(
        int64_t current_pts_us) const;
    void set_video_decode_paused(
        bool paused,
        const std::function<void(size_t slot, TrackPipeline& track, bool paused)>&
            set_decode_paused);
    void set_decode_paused_for_all(bool paused,
                                   const TrackDecodePauseHooks& hooks);
    void apply_playback_decode_state(bool playback_active,
                                     const TrackPlaybackDecodeStateHooks& hooks);
    bool apply_track_offset(int file_id,
                            int64_t offset_us,
                            const TrackOffsetMutationHooks& hooks);
    StepDecisionBuildResult build_step_forward_decision(
        int64_t current_pts_us,
        const PresentDecision& last_decision,
        PresentDecision& decision) const;
    StepDecisionApplication apply_step_forward_decision(
        int64_t current_pts_us,
        const PresentDecision& decision,
        const PresentDecision& last_decision);
    void discard_step_forward_consumed_frames(
        int64_t current_pts_us,
        const PresentDecision& decision,
        const PresentDecision& last_decision);
    StepForwardExactSeekTarget choose_step_forward_exact_seek_target(
        int64_t clock_pts_us,
        const PresentDecision& last_decision,
        std::optional<int64_t> logical_step_anchor_us = std::nullopt) const;
    bool build_step_backward_decision(int64_t current_pts_us,
                                      const PresentDecision& last_decision,
                                      PresentDecision& decision) const;
    StepDecisionApplication apply_step_backward_decision(
        const PresentDecision& decision);
    StepBackwardExactSeekTarget choose_step_backward_exact_seek_target(
        int64_t clock_pts_us,
        const PresentDecision& last_decision) const;
    StepBackwardReconstructionPlan build_step_backward_reconstruction_plan(
        int64_t clock_pts_us,
        const PresentDecision& last_decision) const;
    void apply_carry_forward(const PresentDecision& last_decision,
                             PresentDecision& decision) const;
    void filter_present_decision(PresentDecision& decision) const;
    std::vector<SeekPreviewPresentedTrackEvent> collect_seek_preview_presented_events(
        const PresentDecision& decision,
        int64_t request_id,
        int64_t target_pts_us) const;
    std::vector<LayoutTrackGeometryUpdate> update_layout_track_geometry_from_decision(
        const PresentDecision& decision);
    PlaybackEofBoundary playback_eof_boundary(
        const PresentDecision& last_decision) const;
    std::optional<int64_t> next_frame_event_pts_us(int64_t current_pts_us) const;
    RendererPausedCachedDecision paused_cached_decision(
        const PresentDecision& last_decision) const;
    RendererPausedLayoutDecision paused_layout_decision(
        const PresentDecision& last_decision) const;
    RendererPausedRefreshDecision paused_refresh_decision(
        const PresentDecision& last_decision,
        const std::optional<PresentDecision>& evaluated_decision,
        bool decoded_preview_refresh) const;
    bool has_complete_cached_decision(
        const PresentDecision& last_decision) const;
    PausedPreviewSnapshot paused_preview_snapshot() const;
    RendererTrackReferenceSnapshot first_active_reference() const;

    int next_file_id() const;
    void set_next_file_id(int value);
    int allocate_next_file_id();

    uint64_t allocate_generation();
    void reset_ids();
    InitialTrackOpenResult open_initial_tracks(
        const std::vector<std::string>& video_paths,
        bool use_hardware_decode,
        const InitialTrackOpenHooks& hooks,
        const char* log_context);
    void bind_to_render_sink(RenderSink& render_sink) const;
    void stop_all(const RendererTrackBeforeStopCallback& before_stop = {});
    RendererTrackAddReservation reserve_add_track(int requested_file_id);
    bool can_commit_add(size_t slot) const;
    TrackPipeline* commit_new_track(size_t slot,
                                    std::unique_ptr<TrackPipeline> pipeline,
                                    const TrackAddCommitHooks& hooks);
    RendererTrackDetachResult detach_and_compact_by_file_id(
        int file_id,
        const RendererTrackDetachHooks& hooks);
    RendererTrackRecreateDetachResult detach_for_recreate(
        size_t slot,
        const RendererTrackRecreateDetachHooks& hooks);
    bool can_commit_recreated_track(size_t slot) const;
    TrackPipeline* commit_recreated_track(size_t slot,
                                          std::unique_ptr<TrackPipeline> pipeline,
                                          const TrackAddCommitHooks& hooks);
    std::vector<RendererTrackSeekApplicationResult> apply_seek_to_all(
        int64_t target_pts_us,
        SeekType type,
        bool playing,
        bool force_recreate_paused_hevc,
        const RendererTrackSeekHooks& hooks,
        const StepBackwardTrackSeekTargets* target_overrides = nullptr);
    bool apply_seek_to_all_and_log(int64_t target_pts_us,
                                   SeekType type,
                                   bool playing,
                                   bool force_recreate_paused_hevc,
                                   const RendererTrackSeekHooks& hooks,
                                   const StepBackwardTrackSeekTargets*
                                       target_overrides = nullptr);

    int64_t cached_duration_us() const;
    void set_cached_duration_us(int64_t duration_us);
    void extend_cached_duration_with(const TrackPipeline& track);
    void recompute_cached_duration();
    int64_t effective_duration_us() const;
    int64_t offset_us_for_file_id(int file_id) const;
    TrackAddSeekResult prepare_add_track_seek_to_clock(
        TrackPipeline& track,
        int64_t current_pts_us,
        bool was_playing,
        const TrackAddSeekHooks& hooks) const;

    void reset_perf_baseline(std::chrono::steady_clock::time_point now);
    void reset_perf_baseline();

private:
    std::unique_ptr<RendererTrackRegistry> registry_;
    std::unique_ptr<RendererTrackMutationController> mutation_;
    std::unique_ptr<RendererTrackPresentationModel> presentation_model_;
};

} // namespace vr
