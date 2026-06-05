#pragma once

#include "renderer/track/renderer_track_controller.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace vr {

class RendererTrackRegistry;

// Owns track mutation operations that may detach, compact, recreate, pause, or
// seek pipelines. It does not own track storage or presentation snapshots.
class RendererTrackMutationController {
public:
    explicit RendererTrackMutationController(RendererTrackRegistry& registry);

    bool configure_and_start_pipeline(TrackPipeline& pipeline,
                                      const TrackPipelineStartConfig& config,
                                      const TrackPipelineStartHooks& hooks,
                                      const char* log_context) const;
    void stop_detached_pipeline(size_t slot,
                                std::unique_ptr<TrackPipeline>& track) const;

    void set_video_decode_paused(
        bool paused,
        const std::function<void(size_t slot, TrackPipeline& track, bool paused)>&
            set_decode_paused);
    void set_decode_paused_for_all(bool paused,
                                   const TrackDecodePauseHooks& hooks);
    void apply_playback_decode_state(
        bool playback_active,
        const TrackPlaybackDecodeStateHooks& hooks);
    bool apply_track_offset(int file_id,
                            int64_t offset_us,
                            const TrackOffsetMutationHooks& hooks);

    StepDecisionApplication apply_step_forward_decision(
        int64_t current_pts_us,
        const PresentDecision& decision,
        const PresentDecision& last_decision);
    void discard_step_forward_consumed_frames(
        int64_t current_pts_us,
        const PresentDecision& decision,
        const PresentDecision& last_decision);
    StepDecisionApplication apply_step_backward_decision(
        const PresentDecision& decision);

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
        const RendererTrackSeekHooks& hooks);
    bool apply_seek_to_all_and_log(int64_t target_pts_us,
                                   SeekType type,
                                   bool playing,
                                   bool force_recreate_paused_hevc,
                                   const RendererTrackSeekHooks& hooks);

private:
    RendererTrackRegistry& registry_;
};

} // namespace vr
