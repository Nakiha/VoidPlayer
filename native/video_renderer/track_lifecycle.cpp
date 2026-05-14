#include "video_renderer/track_lifecycle.h"

#include <spdlog/spdlog.h>

#include <optional>
#include <utility>

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

void remove_and_compact_track_pipeline(
    TrackPipelineManager& tracks,
    size_t slot,
    const TrackRemovalHooks& hooks) {
    tracks.stop_slot(slot, [&](size_t stopped_slot, TrackPipeline& track) {
        if (hooks.unregister_audio) {
            hooks.unregister_audio(track.file_id);
        }
        if (hooks.clear_slot) {
            hooks.clear_slot(stopped_slot, track);
        }
    });

    tracks.compact_from(slot, [&](size_t from, size_t to, TrackPipeline& track) {
        if (hooks.move_slot) {
            hooks.move_slot(from, to, track);
        }
    });
}

void compact_present_decision_frames(PresentDecision& decision, size_t slot) {
    if (slot >= kMaxTracks) {
        return;
    }
    for (size_t i = slot; i < kMaxTracks - 1; ++i) {
        decision.frames[i] = std::move(decision.frames[i + 1]);
    }
    decision.frames[kMaxTracks - 1] = std::nullopt;
}

} // namespace vr
