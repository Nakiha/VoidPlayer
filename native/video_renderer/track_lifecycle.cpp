#include "video_renderer/track_lifecycle.h"

#include <spdlog/spdlog.h>

namespace vr {

bool configure_and_start_track_pipeline(
    TrackPipeline& pipeline,
    const TrackPipelineStartConfig& config,
    const TrackPipelineStartHooks& hooks,
    const char* log_context) {
    pipeline.file_id = config.file_id;
    pipeline.offset_us = config.offset_us;
    pipeline.recreated_for_paused_hevc_seek = config.recreated_for_paused_hevc_seek;
    if (pipeline.decode_thread) {
        pipeline.decode_thread->set_pause_after_preroll(config.pause_after_preroll);
    }

    if (hooks.configure_seek_callback) {
        hooks.configure_seek_callback(pipeline);
    }
    if (hooks.configure_error_callback) {
        hooks.configure_error_callback(pipeline);
    }
    if (hooks.register_audio) {
        hooks.register_audio(pipeline);
    }

    if (!pipeline.demux_thread || !pipeline.demux_thread->start_thread()) {
        spdlog::error("{}: failed to start demux thread for {}",
                      log_context ? log_context : "TrackPipeline",
                      pipeline.file_path);
        if (hooks.unregister_audio) {
            hooks.unregister_audio(pipeline.file_id);
        }
        if (pipeline.decode_thread) {
            pipeline.decode_thread->stop();
        }
        if (pipeline.demux_thread) {
            pipeline.demux_thread->stop();
        }
        return false;
    }

    return true;
}

} // namespace vr
