#pragma once

#include "video_renderer/track_pipeline.h"

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

} // namespace vr
