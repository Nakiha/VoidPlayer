#include "video_renderer/track_snapshot.h"

#include <utility>

namespace vr {

std::vector<TrackInfo> snapshot_track_infos(const TrackPipelineManager& tracks) {
    std::vector<TrackInfo> infos;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }

        const auto& track = *tracks[i];
        const auto* demux = track.demux_thread.get();
        const auto* decode = track.decode_thread.get();
        TrackInfo info;
        info.file_id = track.file_id;
        info.slot = static_cast<int>(i);
        info.file_path = track.file_path;
        info.width = track.video_width;
        info.height = track.video_height;
        if (demux) {
            const auto& stats = demux->stats();
            info.duration_us = stats.duration_us;
            info.start_time_us = stats.start_time_us;
            info.bit_rate = stats.bit_rate;
            info.format_name = stats.format_name;
            info.codec_name = stats.codec_name;
            info.codec_long_name = stats.codec_long_name;
        }
        if (decode) {
            info.decoder_name = decode->decoder_name();
        }
        infos.push_back(std::move(info));
    }
    return infos;
}

std::vector<RenderLoopTrackDiagnosticSnapshot>
snapshot_render_loop_track_diagnostics(const TrackPipelineManager& tracks) {
    std::vector<RenderLoopTrackDiagnosticSnapshot> snapshots;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }

        RenderLoopTrackDiagnosticSnapshot snapshot;
        snapshot.slot = i;
        if (tracks[i]->track_buffer) {
            snapshot.buffer_count = tracks[i]->track_buffer->total_count();
            snapshot.buffer_capacity = tracks[i]->track_buffer->preroll_target();
            snapshot.buffer_state = tracks[i]->track_buffer->state();
        }
        snapshots.push_back(snapshot);
    }
    return snapshots;
}

TrackPerfSnapshotResult snapshot_track_perf_stats(
    size_t slot,
    const TrackPipeline& track,
    const DecodePerfCounters::Snapshot& decode_perf,
    const std::optional<TextureFrame>& current_frame,
    uint64_t baseline_frames,
    double elapsed_s) {
    TrackPerfStats stats;
    stats.slot = static_cast<int>(slot);
    stats.file_id = track.file_id;
    if (track.track_buffer) {
        stats.buffer_count = track.track_buffer->total_count();
        stats.buffer_capacity = track.track_buffer->preroll_target();
        stats.buffer_state = track.track_buffer->state();
    }
    if (current_frame.has_value()) {
        stats.current_pts_us = current_frame->pts_us;
        stats.current_dts_us = current_frame->dts_us;
    }

    if (decode_perf.frames_decoded > 0) {
        stats.avg_decode_ms = static_cast<double>(decode_perf.total_decode_us) /
                              static_cast<double>(decode_perf.frames_decoded) /
                              1000.0;
    }
    stats.max_decode_ms = static_cast<double>(decode_perf.max_decode_us) / 1000.0;

    const uint64_t delta_frames = decode_perf.frames_decoded - baseline_frames;
    if (elapsed_s > 0.5) {
        stats.fps = static_cast<double>(delta_frames) / elapsed_s;
    }

    return {stats, decode_perf.frames_decoded};
}

TrackPerfStatsCollectionResult snapshot_track_perf_stats_collection(
    const TrackPipelineManager& tracks,
    const PresentDecision& last_decision,
    const TrackPerfBaselineTracker& baseline_tracker,
    double elapsed_s) {
    TrackPerfStatsCollectionResult result;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }

        const auto& track = *tracks[i];
        DecodePerfCounters::Snapshot decode_perf{};
        if (track.decode_thread) {
            decode_perf = track.decode_thread->perf_counters().snapshot();
        }
        const auto snapshot = snapshot_track_perf_stats(
            i, track, decode_perf, last_decision.frames[i],
            baseline_tracker.baseline_frames(i), elapsed_s);
        result.frames_decoded_by_slot[i] = snapshot.frames_decoded;
        result.stats.push_back(snapshot.stats);
    }
    return result;
}

TrackGpuMemoryStats snapshot_track_gpu_memory_stats(
    size_t slot,
    const TrackPipeline& track,
    const DecodeMemoryStats* decode_stats,
    uint64_t presenter_copy_texture_bytes) {
    TrackGpuMemoryStats stats;
    stats.slot = static_cast<int>(slot);
    stats.file_id = track.file_id;
    if (track.track_buffer) {
        stats.buffer_count = track.track_buffer->total_count();
        stats.buffer_capacity = track.track_buffer->max_count();
        stats.track_buffer_cpu_bytes = track.track_buffer->estimated_cpu_bytes();
    }
    if (track.packet_queue) {
        stats.packet_queue_bytes = track.packet_queue->estimated_bytes();
    }
    if (decode_stats) {
        stats.hardware_enabled = decode_stats->hardware_enabled;
        stats.hardware_download_to_cpu = decode_stats->hardware_download_to_cpu;
        stats.hw_format = decode_stats->hw_format;
        stats.sw_format = decode_stats->sw_format;
        stats.hw_width = decode_stats->hw_width;
        stats.hw_height = decode_stats->hw_height;
        stats.hw_initial_pool_size = decode_stats->hw_initial_pool_size;
        stats.extra_hw_frames = decode_stats->extra_hw_frames;
        stats.decoder_frame_bytes = decode_stats->estimated_hw_frame_bytes;
        stats.decoder_pool_bytes = decode_stats->estimated_hw_pool_bytes;
        stats.exact_seek_snapshot_bytes = decode_stats->snapshot_pool.estimated_bytes;
        stats.exact_seek_candidate_cpu_bytes =
            decode_stats->exact_seek_candidate_cpu_bytes;
        stats.exact_seek_stable_cpu_bytes =
            decode_stats->exact_seek_stable_cpu_bytes;
        stats.exact_seek_reorder_count = decode_stats->exact_seek_reorder_count;
        stats.exact_seek_pending_count = decode_stats->exact_seek_pending_count;
        stats.exact_seek_stable_frame_count =
            decode_stats->exact_seek_stable_frame_count;
    }
    stats.presenter_copy_texture_bytes = presenter_copy_texture_bytes;
    stats.total_cpu_frame_bytes =
        stats.track_buffer_cpu_bytes +
        stats.exact_seek_candidate_cpu_bytes +
        stats.exact_seek_stable_cpu_bytes;
    return stats;
}

TrackGpuMemoryStatsCollectionResult snapshot_track_gpu_memory_stats_collection(
    const TrackPipelineManager& tracks,
    const std::array<uint64_t, kMaxTracks>& presenter_copy_texture_bytes_by_slot) {
    TrackGpuMemoryStatsCollectionResult result;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }

        std::optional<DecodeMemoryStats> decode_stats;
        if (tracks[i]->decode_thread) {
            decode_stats = tracks[i]->decode_thread->memory_stats();
        }
        auto track = snapshot_track_gpu_memory_stats(
            i, *tracks[i], decode_stats ? &*decode_stats : nullptr,
            presenter_copy_texture_bytes_by_slot[i]);

        result.decoder_pool_bytes += track.decoder_pool_bytes;
        result.exact_seek_snapshot_bytes += track.exact_seek_snapshot_bytes;
        result.track_buffer_cpu_bytes += track.track_buffer_cpu_bytes;
        result.packet_queue_bytes += track.packet_queue_bytes;
        result.exact_seek_candidate_cpu_bytes +=
            track.exact_seek_candidate_cpu_bytes;
        result.exact_seek_stable_cpu_bytes += track.exact_seek_stable_cpu_bytes;
        result.cpu_frame_bytes += track.total_cpu_frame_bytes;
        result.total_estimated_bytes +=
            track.decoder_pool_bytes + track.exact_seek_snapshot_bytes;
        result.tracks.push_back(std::move(track));
    }
    return result;
}

} // namespace vr
