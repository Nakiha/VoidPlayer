#pragma once

#include "video_renderer/track_pipeline.h"
#include "video_renderer/sync/render_sink.h"

#include <cstdint>
#include <functional>

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

} // namespace vr
