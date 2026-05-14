#include "video_renderer/track_lifecycle.h"

#include <spdlog/spdlog.h>

#include <algorithm>
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

int64_t track_pts_end_us_from_stats(const DemuxStats& stats) {
    if (stats.duration_us <= 0) {
        return 0;
    }
    if (stats.start_time_us <= 0) {
        return stats.duration_us;
    }
    // Most containers expose duration as a span. Some FLV files expose a value
    // closer to the absolute end PTS; detect those so a -start offset maps the
    // track to its actual playable span instead of the full PTS epoch.
    if (stats.duration_us > stats.start_time_us &&
        stats.duration_us - stats.start_time_us < stats.duration_us / 2) {
        return stats.duration_us;
    }
    return stats.start_time_us + stats.duration_us;
}

int64_t clamp_track_seek_target_us(const TrackPipeline& track,
                                   int64_t target_pts_us) {
    const int64_t track_target =
        std::max(target_pts_us - track.offset_us, int64_t(0));
    if (!track.demux_thread) {
        return track_target;
    }

    const int64_t track_end_us =
        track_pts_end_us_from_stats(track.demux_thread->stats());
    if (track_end_us <= 0) {
        return track_target;
    }

    return std::min(track_target, track_end_us);
}

TrackAddSeekResult prepare_add_track_seek_to_clock(
    TrackPipeline& track,
    int64_t current_pts_us,
    bool was_playing,
    const TrackAddSeekHooks& hooks) {
    TrackAddSeekResult result;
    if (current_pts_us <= 0) {
        return result;
    }

    result.target_pts_us = clamp_track_seek_target_us(track, current_pts_us);
    result.seek_type = was_playing ? SeekType::Keyframe : SeekType::Exact;

    if (track.decode_thread) {
        track.decode_thread->set_decode_paused(true);
    }
    track.track_buffer->set_state(TrackState::Flushing);
    track.track_buffer->clear_frames();
    track.packet_queue->flush();
    if (track.audio_packet_queue) {
        track.audio_packet_queue->flush();
    }
    if (hooks.set_audio_decode_paused) {
        hooks.set_audio_decode_paused(track.file_id, true);
    }
    track.seek_controller->request_seek(result.target_pts_us, result.seek_type);
    track.track_buffer->set_state(TrackState::Buffering);
    result.applied = true;
    return result;
}

TrackSeekPreparationResult prepare_track_seek_transition(
    TrackPipeline& track,
    const TrackSeekPreparationConfig& config,
    const TrackSeekPreparationHooks& hooks) {
    TrackSeekPreparationResult result;
    if (track.decode_thread) {
        track.decode_thread->set_decode_paused(true);
    }
    if (hooks.set_audio_decode_paused) {
        hooks.set_audio_decode_paused(track.file_id, true);
    }

    result.buffered_frames_before = track.track_buffer->total_count();
    result.packet_queue_size_before = track.packet_queue->size();
    result.buffer_state_before = track.track_buffer->state();
    result.seek_transition_active =
        result.buffer_state_before == TrackState::Flushing ||
        result.buffer_state_before == TrackState::Buffering;

    track.track_buffer->set_state(TrackState::Flushing);
    track.track_buffer->clear_frames();
    if (config.reset_presenter_track && hooks.reset_presenter_track) {
        hooks.reset_presenter_track();
    }
    track.packet_queue->flush();
    if (track.audio_packet_queue) {
        track.audio_packet_queue->flush();
    }

    return result;
}

void submit_track_seek_after_recreate(
    TrackPipeline& track,
    int64_t target_pts_us,
    SeekType type,
    bool paused_seek,
    bool recreated_for_seek) {
    if (track.decode_thread) {
        track.decode_thread->set_pause_after_preroll(paused_seek);
    }
    if (!recreated_for_seek) {
        track.seek_controller->request_seek(target_pts_us, type);
    }
}

} // namespace vr
