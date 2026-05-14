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

} // namespace vr
