#include "video_renderer/track_lifecycle.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace vr {
namespace {

bool is_h264_flv_track(const TrackPipeline& track) {
    if (!track.demux_thread) {
        return false;
    }
    const auto& stats = track.demux_thread->stats();
    if (!stats.codec_params || stats.codec_params->codec_id != AV_CODEC_ID_H264) {
        return false;
    }
    std::string format = stats.format_name;
    std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return format.find("flv") != std::string::npos;
}

} // namespace

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

InitialTrackOpenResult open_initial_track_pipelines(
    TrackPipelineManager& tracks,
    const std::vector<std::string>& video_paths,
    bool use_hardware_decode,
    const InitialTrackOpenHooks& hooks,
    const char* log_context) {
    InitialTrackOpenResult result;
    const char* context = log_context ? log_context : "TrackLifecycle";

    for (const auto& path : video_paths) {
        const int slot = tracks.find_empty_slot();
        if (slot < 0) {
            spdlog::warn("{}: skipping {}, max {} tracks",
                         context, path, kMaxTracks);
            ++result.skipped_full_count;
            continue;
        }

        if (!hooks.create_pipeline) {
            ++result.failed_pipeline_count;
            continue;
        }
        auto pipeline = hooks.create_pipeline(path, use_hardware_decode);
        if (!pipeline) {
            ++result.failed_pipeline_count;
            continue;
        }

        const int file_id = hooks.allocate_file_id ? hooks.allocate_file_id() : 0;
        const TrackPipelineStartConfig start_config{
            file_id,
            0,
            true,
            false,
        };
        if (!configure_and_start_track_pipeline(
                *pipeline, start_config, hooks.start_hooks, context)) {
            ++result.failed_start_count;
            continue;
        }

        tracks[static_cast<size_t>(slot)] = std::move(pipeline);
        ++result.opened_count;
    }

    return result;
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

TrackSeekTargetResolution resolve_track_seek_target(
    const TrackPipeline& track,
    int64_t global_target_pts_us) {
    TrackSeekTargetResolution result;
    result.requested_target_us =
        std::max(global_target_pts_us - track.offset_us, int64_t(0));
    result.target_us = clamp_track_seek_target_us(track, global_target_pts_us);
    result.clamped = result.target_us != result.requested_target_us;
    return result;
}

bool track_uses_hardware_codec(const TrackPipeline& track, AVCodecID codec_id) {
    if (!track.decode_thread) {
        return false;
    }
    return track.decode_thread->is_hardware_decode_enabled() &&
           track.decode_thread->codec_id() == codec_id;
}

bool any_track_uses_hardware_codec(
    const TrackPipelineManager& tracks,
    AVCodecID codec_id) {
    for (const auto& track : tracks) {
        if (track && track_uses_hardware_codec(*track, codec_id)) {
            return true;
        }
    }
    return false;
}

TrackSeekFacts inspect_track_seek_facts(
    const TrackPipeline& track,
    int64_t global_target_pts_us,
    SeekType type) {
    TrackSeekFacts facts;
    facts.target = resolve_track_seek_target(track, global_target_pts_us);
    facts.warn_h264_flv_exact_seek =
        type == SeekType::Exact && is_h264_flv_track(track);
    facts.hardware_decode_enabled =
        track.decode_thread && track.decode_thread->is_hardware_decode_enabled();
    facts.hevc_hardware_seek = track_uses_hardware_codec(track, AV_CODEC_ID_HEVC);
    return facts;
}

TrackOffsetMutationResult apply_track_offset_mutation(
    TrackPipeline& track,
    size_t slot,
    int64_t offset_us,
    const TrackOffsetMutationHooks& hooks) {
    TrackOffsetMutationResult result;
    result.previous_offset_us = track.offset_us;
    result.offset_us = offset_us;
    result.changed = result.previous_offset_us != offset_us;
    track.offset_us = offset_us;
    if (hooks.set_render_track_offset) {
        hooks.set_render_track_offset(slot, offset_us);
    }
    return result;
}

int64_t track_duration_us(const TrackPipeline& track) {
    if (!track.demux_thread) {
        return 0;
    }
    return std::max<int64_t>(0, track.demux_thread->stats().duration_us);
}

int64_t extend_track_duration_cache(int64_t cached_duration_us,
                                    const TrackPipeline& track) {
    return std::max(cached_duration_us, track_duration_us(track));
}

int64_t compute_track_duration_cache(const TrackPipelineManager& tracks) {
    int64_t duration_us = 0;
    for (const auto& track : tracks) {
        if (track) {
            duration_us = extend_track_duration_cache(duration_us, *track);
        }
    }
    return duration_us;
}

int64_t resolve_effective_duration_us(const TrackPipelineManager& tracks,
                                      int64_t cached_duration_us) {
    int64_t duration_us = 0;
    bool has_track_duration = false;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i] || !tracks[i]->demux_thread) {
            continue;
        }
        const int64_t track_pts_end_us =
            track_pts_end_us_from_stats(tracks[i]->demux_thread->stats());
        if (track_pts_end_us <= 0) {
            continue;
        }
        has_track_duration = true;
        duration_us = std::max(duration_us, track_pts_end_us + tracks[i]->offset_us);
    }
    if (!has_track_duration) {
        duration_us = cached_duration_us;
    }
    return std::max<int64_t>(0, duration_us);
}

TrackPlaybackMutationState pause_playback_for_track_mutation(
    bool currently_playing,
    const TrackPlaybackMutationHooks& hooks) {
    TrackPlaybackMutationState state;
    state.was_playing = currently_playing;
    if (!state.was_playing) {
        return state;
    }

    if (hooks.pause_playback) {
        hooks.pause_playback();
    }
    if (hooks.set_playing) {
        hooks.set_playing(false);
    }
    return state;
}

void rollback_track_mutation_playback(
    const TrackPlaybackMutationState& state,
    const TrackPlaybackMutationHooks& hooks) {
    if (!state.was_playing) {
        return;
    }

    if (hooks.resume_playback) {
        hooks.resume_playback();
    }
    if (hooks.set_playing) {
        hooks.set_playing(true);
    }
}

void finish_track_removal_playback(
    const TrackPlaybackMutationState& state,
    bool has_active_tracks,
    const TrackPlaybackMutationHooks& hooks) {
    if (!state.was_playing || !has_active_tracks) {
        return;
    }

    if (hooks.resume_playback) {
        hooks.resume_playback();
    }
    if (hooks.set_playing) {
        hooks.set_playing(true);
    }
}

void apply_track_decode_pause_state(
    TrackPipelineManager& tracks,
    bool paused,
    const TrackDecodePauseHooks& hooks) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        if (hooks.set_decode_paused) {
            hooks.set_decode_paused(i, *tracks[i], paused);
        }
    }

    if (hooks.set_all_audio_decode_paused) {
        hooks.set_all_audio_decode_paused(paused);
    }
}

void apply_track_playback_decode_state(
    TrackPipelineManager& tracks,
    bool playback_active,
    const TrackPlaybackDecodeStateHooks& hooks) {
    const bool paused = !playback_active;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        if (hooks.set_pause_after_preroll) {
            hooks.set_pause_after_preroll(i, *tracks[i], paused);
        }
    }

    apply_track_decode_pause_state(
        tracks,
        paused,
        TrackDecodePauseHooks{
            hooks.set_decode_paused,
            hooks.set_all_audio_decode_paused,
        });
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

TrackPipeline* commit_new_track_pipeline(
    TrackPipelineManager& tracks,
    size_t slot,
    std::unique_ptr<TrackPipeline> pipeline,
    const TrackAddCommitHooks& hooks) {
    if (slot >= kMaxTracks || !pipeline) {
        return nullptr;
    }

    if (hooks.set_render_slot) {
        hooks.set_render_slot(slot, *pipeline);
    }
    if (hooks.reset_presenter_track) {
        hooks.reset_presenter_track(slot);
    }
    tracks[slot] = std::move(pipeline);
    return tracks[slot].get();
}

void bind_existing_tracks_to_render_sink(
    const TrackPipelineManager& tracks,
    RenderSink& render_sink) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        render_sink.set_track(i, tracks[i]->track_buffer);
    }
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

TrackSeekTransitionPlan build_track_seek_transition_plan(
    const TrackPipeline& track,
    const TrackSeekFacts& facts,
    const TrackSeekPreparationResult& preparation,
    bool playing,
    bool force_recreate_paused_hevc,
    SeekType type) {
    TrackSeekTransitionPlan plan;
    plan.paused_seek = !playing;
    plan.seek_type = type;
    plan.hevc_recreate_input.is_hevc_hw_seek = facts.hevc_hardware_seek;
    plan.hevc_recreate_input.paused_seek = plan.paused_seek;
    plan.hevc_recreate_input.seek_transition_active =
        preparation.seek_transition_active;
    plan.hevc_recreate_input.recreated_for_paused_hevc_seek =
        track.recreated_for_paused_hevc_seek;
    plan.hevc_recreate_input.force_recreate_paused_hevc =
        force_recreate_paused_hevc;
    plan.hevc_recreate_input.seek_type = type;
    return plan;
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

bool recreate_track_pipeline_for_seek(
    TrackPipelineManager& tracks,
    size_t slot,
    int64_t target_pts_us,
    SeekType type,
    const TrackPipelineRecreateHooks& hooks,
    const char* log_context) {
    auto& current = tracks[slot];
    if (!current) {
        return false;
    }

    const char* context = log_context ? log_context : "TrackLifecycle";
    spdlog::info("{} Recreating pipeline for {}", context, current->file_path);

    const auto file_path = current->file_path;
    const auto file_id = current->file_id;
    const auto offset_us = current->offset_us;
    const auto use_hardware_decode = current->use_hardware_decode;

    if (hooks.unregister_audio) {
        hooks.unregister_audio(file_id);
    }
    if (hooks.clear_slot) {
        hooks.clear_slot(slot);
    }
    if (hooks.reset_presenter_track) {
        hooks.reset_presenter_track(slot);
    }
    tracks.stop_slot(slot);

    // Give the driver a brief moment to retire the previous D3D11VA decoder
    // objects before constructing a fresh hardware pipeline on the same file.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    if (!hooks.create_pipeline) {
        spdlog::error("{} Failed to recreate pipeline for {}", context, file_path);
        return false;
    }
    const SeekRequest initial_seek{target_pts_us, type};
    auto replacement =
        hooks.create_pipeline(file_path, use_hardware_decode, initial_seek);
    if (!replacement) {
        spdlog::error("{} Failed to recreate pipeline for {}", context, file_path);
        return false;
    }

    const TrackPipelineStartConfig start_config{
        file_id,
        offset_us,
        false,
        true,
    };
    if (!configure_and_start_track_pipeline(
            *replacement, start_config, hooks.start_hooks, context)) {
        return false;
    }

    if (hooks.commit_slot) {
        hooks.commit_slot(slot, *replacement);
    }
    tracks[slot] = std::move(replacement);
    return true;
}

} // namespace vr
