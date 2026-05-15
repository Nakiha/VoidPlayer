#pragma once

#include "video_renderer/track/track_pipeline.h"
#include "video_renderer/seek/seek_coordinator.h"
#include "video_renderer/sync/render_sink.h"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vr {

struct TrackPipelineStartConfig {
    int file_id = 0;
    int64_t offset_us = 0;
    bool pause_after_preroll = true;
    bool recreated_for_paused_hevc_seek = false;
};

struct TrackPipelineStartHooks {
    std::function<void(TrackPipeline&)> configure_seek_callback;
    std::function<void(TrackPipeline&)> configure_error_callback;
    std::function<void(TrackPipeline&)> register_audio;
    std::function<void(int file_id)> unregister_audio;
};

bool configure_and_start_track_pipeline(
    TrackPipeline& pipeline,
    const TrackPipelineStartConfig& config,
    const TrackPipelineStartHooks& hooks,
    const char* log_context);

struct InitialTrackOpenHooks {
    std::function<std::unique_ptr<TrackPipeline>(
        const std::string& path,
        bool use_hardware_decode)> create_pipeline;
    std::function<int()> allocate_file_id;
    TrackPipelineStartHooks start_hooks;
};

struct InitialTrackOpenResult {
    size_t opened_count = 0;
    size_t skipped_full_count = 0;
    size_t failed_pipeline_count = 0;
    size_t failed_start_count = 0;
};

InitialTrackOpenResult open_initial_track_pipelines(
    TrackPipelineManager& tracks,
    const std::vector<std::string>& video_paths,
    bool use_hardware_decode,
    const InitialTrackOpenHooks& hooks,
    const char* log_context);

struct TrackRemovalHooks {
    std::function<void(int file_id)> unregister_audio;
    std::function<void(size_t slot, TrackPipeline& track)> clear_slot;
    std::function<void(size_t from, size_t to, TrackPipeline& track)> move_slot;
};

void remove_and_compact_track_pipeline(
    TrackPipelineManager& tracks,
    size_t slot,
    const TrackRemovalHooks& hooks);

void compact_present_decision_frames(PresentDecision& decision, size_t slot);

int64_t track_pts_end_us_from_stats(const DemuxStats& stats);

int64_t clamp_track_seek_target_us(const TrackPipeline& track,
                                   int64_t target_pts_us);

struct TrackSeekTargetResolution {
    int64_t requested_target_us = 0;
    int64_t target_us = 0;
    bool clamped = false;
};

struct TrackSeekFacts {
    TrackSeekTargetResolution target;
    bool warn_h264_flv_exact_seek = false;
    bool hardware_decode_enabled = false;
    bool hevc_hardware_seek = false;
};

TrackSeekTargetResolution resolve_track_seek_target(
    const TrackPipeline& track,
    int64_t global_target_pts_us);

bool track_uses_hardware_codec(const TrackPipeline& track, AVCodecID codec_id);

bool any_track_uses_hardware_codec(
    const TrackPipelineManager& tracks,
    AVCodecID codec_id);

TrackSeekFacts inspect_track_seek_facts(
    const TrackPipeline& track,
    int64_t global_target_pts_us,
    SeekType type);

struct TrackOffsetMutationHooks {
    std::function<void(size_t slot, int64_t offset_us)> set_render_track_offset;
};

struct TrackOffsetMutationResult {
    int64_t previous_offset_us = 0;
    int64_t offset_us = 0;
    bool changed = false;
};

TrackOffsetMutationResult apply_track_offset_mutation(
    TrackPipeline& track,
    size_t slot,
    int64_t offset_us,
    const TrackOffsetMutationHooks& hooks);

int64_t track_duration_us(const TrackPipeline& track);

int64_t extend_track_duration_cache(int64_t cached_duration_us,
                                    const TrackPipeline& track);

int64_t compute_track_duration_cache(const TrackPipelineManager& tracks);

int64_t resolve_effective_duration_us(const TrackPipelineManager& tracks,
                                      int64_t cached_duration_us);

struct TrackPlaybackMutationHooks {
    std::function<void()> pause_playback;
    std::function<void()> resume_playback;
    std::function<void(bool playing)> set_playing;
};

struct TrackPlaybackMutationState {
    bool was_playing = false;
};

TrackPlaybackMutationState pause_playback_for_track_mutation(
    bool currently_playing,
    const TrackPlaybackMutationHooks& hooks);

void rollback_track_mutation_playback(
    const TrackPlaybackMutationState& state,
    const TrackPlaybackMutationHooks& hooks);

void finish_track_removal_playback(
    const TrackPlaybackMutationState& state,
    bool has_active_tracks,
    const TrackPlaybackMutationHooks& hooks);

struct TrackPlaybackDecodeStateHooks {
    std::function<void(size_t slot, TrackPipeline& track, bool enabled)>
        set_pause_after_preroll;
    std::function<void(size_t slot, TrackPipeline& track, bool paused)>
        set_decode_paused;
    std::function<void(bool paused)> set_all_audio_decode_paused;
};

struct TrackDecodePauseHooks {
    std::function<void(size_t slot, TrackPipeline& track, bool paused)>
        set_decode_paused;
    std::function<void(bool paused)> set_all_audio_decode_paused;
};

void apply_track_decode_pause_state(
    TrackPipelineManager& tracks,
    bool paused,
    const TrackDecodePauseHooks& hooks);

void apply_track_playback_decode_state(
    TrackPipelineManager& tracks,
    bool playback_active,
    const TrackPlaybackDecodeStateHooks& hooks);

struct TrackAddSeekHooks {
    std::function<void(int file_id, bool paused)> set_audio_decode_paused;
};

struct TrackAddSeekResult {
    bool applied = false;
    int64_t target_pts_us = 0;
    SeekType seek_type = SeekType::Keyframe;
};

TrackAddSeekResult prepare_add_track_seek_to_clock(
    TrackPipeline& track,
    int64_t current_pts_us,
    bool was_playing,
    const TrackAddSeekHooks& hooks);

struct TrackAddCommitHooks {
    std::function<void(size_t slot, TrackPipeline& track)> set_render_slot;
    std::function<void(size_t slot)> reset_presenter_track;
};

TrackPipeline* commit_new_track_pipeline(
    TrackPipelineManager& tracks,
    size_t slot,
    std::unique_ptr<TrackPipeline> pipeline,
    const TrackAddCommitHooks& hooks);

void bind_existing_tracks_to_render_sink(
    const TrackPipelineManager& tracks,
    RenderSink& render_sink);

struct TrackSeekPreparationConfig {
    bool reset_presenter_track = false;
};

struct TrackSeekPreparationHooks {
    std::function<void(int file_id, bool paused)> set_audio_decode_paused;
    std::function<void()> reset_presenter_track;
};

struct TrackSeekPreparationResult {
    size_t buffered_frames_before = 0;
    size_t packet_queue_size_before = 0;
    TrackState buffer_state_before = TrackState::Empty;
    bool seek_transition_active = false;
};

struct TrackSeekTransitionPlan {
    bool paused_seek = false;
    SeekType seek_type = SeekType::Keyframe;
    HevcSeekRecreateInput hevc_recreate_input;
};

TrackSeekPreparationResult prepare_track_seek_transition(
    TrackPipeline& track,
    const TrackSeekPreparationConfig& config,
    const TrackSeekPreparationHooks& hooks);

TrackSeekTransitionPlan build_track_seek_transition_plan(
    const TrackPipeline& track,
    const TrackSeekFacts& facts,
    const TrackSeekPreparationResult& preparation,
    bool playing,
    bool force_recreate_paused_hevc,
    SeekType type);

struct TrackSeekExecutionResult {
    bool recreated_for_seek = false;
    bool applied_seek = false;
    bool error_state_set = false;
    bool coalescing_transition = false;
};

TrackSeekExecutionResult apply_track_seek_execution_result(
    TrackPipeline& track,
    int64_t target_pts_us,
    const TrackSeekTransitionPlan& plan,
    const HevcSeekRecreateDecision& hevc_recreate_decision,
    bool recreated_for_seek);

struct TrackSeekSlotApplicationHooks {
    TrackSeekPreparationHooks preparation;
    std::function<bool(size_t slot, int64_t target_pts_us, SeekType type)>
        recreate_pipeline_for_seek;
};

struct TrackSeekSlotApplicationResult {
    bool slot_present = false;
    TrackSeekFacts facts;
    TrackSeekPreparationResult preparation;
    TrackSeekTransitionPlan plan;
    HevcSeekRecreateDecision hevc_recreate_decision;
    TrackSeekExecutionResult execution;
};

TrackSeekSlotApplicationResult apply_track_seek_to_slot(
    TrackPipelineManager& tracks,
    size_t slot,
    int64_t global_target_pts_us,
    SeekType type,
    bool playing,
    bool force_recreate_paused_hevc,
    const TrackSeekSlotApplicationHooks& hooks);

void submit_track_seek_after_recreate(
    TrackPipeline& track,
    int64_t target_pts_us,
    SeekType type,
    bool paused_seek,
    bool recreated_for_seek);

struct TrackPipelineRecreateHooks {
    std::function<void(int file_id)> unregister_audio;
    std::function<void(size_t slot)> clear_slot;
    std::function<void(size_t slot)> reset_presenter_track;
    std::function<std::unique_ptr<TrackPipeline>(
        const std::string& path,
        bool use_hardware_decode,
        const SeekRequest& initial_seek)> create_pipeline;
    TrackPipelineStartHooks start_hooks;
    std::function<void(size_t slot, TrackPipeline& track)> commit_slot;
};

bool recreate_track_pipeline_for_seek(
    TrackPipelineManager& tracks,
    size_t slot,
    int64_t target_pts_us,
    SeekType type,
    const TrackPipelineRecreateHooks& hooks,
    const char* log_context);

} // namespace vr
