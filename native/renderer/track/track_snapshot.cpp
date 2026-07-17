#include "renderer/track/track_snapshot.h"

#include <utility>

namespace vr {
namespace {

double avg_ms(uint64_t total_us, uint64_t count) {
    if (count == 0) {
        return 0.0;
    }
    return static_cast<double>(total_us) / static_cast<double>(count) / 1000.0;
}

double avg_value(uint64_t total, uint64_t count) {
    if (count == 0) {
        return 0.0;
    }
    return static_cast<double>(total) / static_cast<double>(count);
}

} // namespace

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
            info.color = stats.color;
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
    const DecodeStagePerfCounters::Snapshot& decode_stage_perf,
    const std::optional<TextureFrame>& current_frame,
    uint64_t baseline_frames,
    double elapsed_s) {
    TrackPerfStats stats;
    stats.slot = static_cast<int>(slot);
    stats.file_id = track.file_id;
    stats.frames_decoded = decode_perf.frames_decoded;
    if (track.track_buffer) {
        stats.buffer_count = track.track_buffer->total_count();
        stats.buffer_capacity = track.track_buffer->max_count();
        stats.buffer_preroll_target = track.track_buffer->preroll_target();
        stats.buffer_state = track.track_buffer->state();
    }
    if (current_frame.has_value()) {
        stats.current_pts_us = current_frame->pts_us;
        stats.current_dts_us = current_frame->dts_us;
        stats.analysis_frame_index = current_frame->analysis_frame_index;
        stats.source_packet_index = current_frame->source_packet_index;
        stats.source_packet_size = current_frame->source_packet_size;
        stats.source_packet_pos = current_frame->source_packet_pos;
        stats.source_packet_pts = current_frame->source_packet_pts;
        stats.source_packet_dts = current_frame->source_packet_dts;
        stats.frame_identity_mode = current_frame->frame_identity_mode;
        stats.current_frame_storage_kind = current_frame->storage_kind();
        if (const auto* yuv = current_frame->cpu_planar_yuv_storage()) {
            stats.current_frame_yuv_bit_depth = yuv->bit_depth;
            stats.current_frame_yuv_plane_layout =
                static_cast<int>(yuv->plane_layout);
            stats.current_frame_yuv_sample_alignment =
                static_cast<int>(yuv->sample_alignment);
        } else if (const auto* nv12 = current_frame->cpu_nv12_storage()) {
            stats.current_frame_yuv_bit_depth = nv12->is_p010 ? 10 : 8;
            stats.current_frame_yuv_plane_layout =
                static_cast<int>(CpuYuvPlaneLayout::SemiPlanarYuv420);
            stats.current_frame_yuv_sample_alignment =
                nv12->is_p010
                    ? static_cast<int>(CpuYuvSampleAlignment::MsbAligned)
                    : static_cast<int>(CpuYuvSampleAlignment::Packed);
        }
    }

    if (decode_perf.frames_decoded > 0) {
        stats.avg_decode_ms = static_cast<double>(decode_perf.total_decode_us) /
                              static_cast<double>(decode_perf.frames_decoded) /
                              1000.0;
    }
    stats.max_decode_ms = static_cast<double>(decode_perf.max_decode_us) / 1000.0;
    stats.decode_stage_packet_send_count = decode_stage_perf.packet_send_count;
    stats.decode_stage_packet_send_avg_ms =
        avg_ms(decode_stage_perf.packet_send_total_us,
               decode_stage_perf.packet_send_count);
    stats.decode_stage_packet_send_max_ms =
        static_cast<double>(decode_stage_perf.packet_send_max_us) / 1000.0;
    stats.decode_stage_receive_loop_count =
        decode_stage_perf.receive_loop_count;
    stats.decode_stage_receive_frame_count =
        decode_stage_perf.receive_loop_frame_count;
    stats.decode_stage_receive_avg_ms =
        avg_ms(decode_stage_perf.receive_loop_total_us,
               decode_stage_perf.receive_loop_frame_count);
    stats.decode_stage_receive_max_ms =
        static_cast<double>(decode_stage_perf.receive_loop_max_us) / 1000.0;
    stats.decode_stage_convert_count = decode_stage_perf.convert_count;
    stats.decode_stage_convert_avg_ms =
        avg_ms(decode_stage_perf.convert_total_us,
               decode_stage_perf.convert_count);
    stats.decode_stage_convert_max_ms =
        static_cast<double>(decode_stage_perf.convert_max_us) / 1000.0;
    stats.decode_stage_convert_direct_planar_count =
        decode_stage_perf.convert_direct_planar_count;
    stats.decode_stage_convert_direct_planar_avg_ms =
        avg_ms(decode_stage_perf.convert_direct_planar_total_us,
               decode_stage_perf.convert_direct_planar_count);
    stats.decode_stage_convert_direct_planar_max_ms =
        static_cast<double>(
            decode_stage_perf.convert_direct_planar_max_us) / 1000.0;
    stats.decode_stage_convert_nv12_layout_count =
        decode_stage_perf.convert_nv12_layout_count;
    stats.decode_stage_convert_nv12_layout_avg_ms =
        avg_ms(decode_stage_perf.convert_nv12_layout_total_us,
               decode_stage_perf.convert_nv12_layout_count);
    stats.decode_stage_convert_nv12_layout_max_ms =
        static_cast<double>(
            decode_stage_perf.convert_nv12_layout_max_us) / 1000.0;
    stats.decode_stage_convert_nv12_alloc_count =
        decode_stage_perf.convert_nv12_alloc_count;
    stats.decode_stage_convert_nv12_alloc_avg_ms =
        avg_ms(decode_stage_perf.convert_nv12_alloc_total_us,
               decode_stage_perf.convert_nv12_alloc_count);
    stats.decode_stage_convert_nv12_alloc_max_ms =
        static_cast<double>(
            decode_stage_perf.convert_nv12_alloc_max_us) / 1000.0;
    stats.decode_stage_convert_nv12_pack_count =
        decode_stage_perf.convert_nv12_pack_count;
    stats.decode_stage_convert_nv12_pack_avg_ms =
        avg_ms(decode_stage_perf.convert_nv12_pack_total_us,
               decode_stage_perf.convert_nv12_pack_count);
    stats.decode_stage_convert_nv12_pack_max_ms =
        static_cast<double>(
            decode_stage_perf.convert_nv12_pack_max_us) / 1000.0;
    stats.decode_stage_publish_count = decode_stage_perf.publish_count;
    stats.decode_stage_publish_avg_ms =
        avg_ms(decode_stage_perf.publish_total_us,
               decode_stage_perf.publish_count);
    stats.decode_stage_publish_max_ms =
        static_cast<double>(decode_stage_perf.publish_max_us) / 1000.0;
    stats.decode_stage_publish_lock_count =
        decode_stage_perf.publish_lock_count;
    stats.decode_stage_publish_lock_avg_ms =
        avg_ms(decode_stage_perf.publish_lock_total_us,
               decode_stage_perf.publish_lock_count);
    stats.decode_stage_publish_lock_max_ms =
        static_cast<double>(decode_stage_perf.publish_lock_max_us) / 1000.0;
    stats.decode_stage_publish_wait_count =
        decode_stage_perf.publish_wait_count;
    stats.decode_stage_publish_wait_avg_ms =
        avg_ms(decode_stage_perf.publish_wait_total_us,
               decode_stage_perf.publish_wait_count);
    stats.decode_stage_publish_wait_max_ms =
        static_cast<double>(decode_stage_perf.publish_wait_max_us) / 1000.0;
    stats.decode_stage_publish_ring_push_count =
        decode_stage_perf.publish_ring_push_count;
    stats.decode_stage_publish_ring_push_avg_ms =
        avg_ms(decode_stage_perf.publish_ring_push_total_us,
               decode_stage_perf.publish_ring_push_count);
    stats.decode_stage_publish_ring_push_max_ms =
        static_cast<double>(
            decode_stage_perf.publish_ring_push_max_us) / 1000.0;
    stats.decode_stage_publish_ring_lock_count =
        decode_stage_perf.publish_ring_lock_count;
    stats.decode_stage_publish_ring_lock_avg_ms =
        avg_ms(decode_stage_perf.publish_ring_lock_total_us,
               decode_stage_perf.publish_ring_lock_count);
    stats.decode_stage_publish_ring_lock_max_ms =
        static_cast<double>(
            decode_stage_perf.publish_ring_lock_max_us) / 1000.0;
    stats.decode_stage_publish_ring_assign_count =
        decode_stage_perf.publish_ring_assign_count;
    stats.decode_stage_publish_ring_assign_avg_ms =
        avg_ms(decode_stage_perf.publish_ring_assign_total_us,
               decode_stage_perf.publish_ring_assign_count);
    stats.decode_stage_publish_ring_assign_max_ms =
        static_cast<double>(
            decode_stage_perf.publish_ring_assign_max_us) / 1000.0;
    stats.decode_stage_publish_ring_advance_count =
        decode_stage_perf.publish_ring_advance_count;
    stats.decode_stage_publish_ring_advance_avg_ms =
        avg_ms(decode_stage_perf.publish_ring_advance_total_us,
               decode_stage_perf.publish_ring_advance_count);
    stats.decode_stage_publish_ring_advance_max_ms =
        static_cast<double>(
            decode_stage_perf.publish_ring_advance_max_us) / 1000.0;
    stats.decode_stage_publish_ring_overwrite_count =
        decode_stage_perf.publish_ring_overwrite_count;
    stats.decode_stage_publish_ring_overwrite_avg_bytes =
        avg_value(decode_stage_perf.publish_ring_overwrite_total_bytes,
                  decode_stage_perf.publish_ring_overwrite_count);
    stats.decode_stage_publish_ring_overwrite_max_bytes =
        decode_stage_perf.publish_ring_overwrite_max_bytes;
    stats.decode_stage_flush_count = decode_stage_perf.flush_count;
    stats.decode_stage_flush_avg_ms =
        avg_ms(decode_stage_perf.flush_total_us,
               decode_stage_perf.flush_count);
    stats.decode_stage_flush_max_ms =
        static_cast<double>(decode_stage_perf.flush_max_us) / 1000.0;

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
        DecodeStagePerfCounters::Snapshot decode_stage_perf{};
        if (track.decode_thread) {
            decode_perf = track.decode_thread->perf_counters().snapshot();
            decode_stage_perf =
                track.decode_thread->stage_perf_counters().snapshot();
        }
        const auto snapshot = snapshot_track_perf_stats(
            i, track, decode_perf, decode_stage_perf, last_decision.frames[i],
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
        stats.snapshot_completion_wait_count =
            decode_stats->snapshot_pool.completion_wait_count;
        stats.snapshot_completion_wait_total_us =
            decode_stats->snapshot_pool.completion_wait_total_us;
        stats.snapshot_completion_wait_max_us =
            decode_stats->snapshot_pool.completion_wait_max_us;
        stats.snapshot_completion_wait_over_budget_count =
            decode_stats->snapshot_pool.completion_wait_over_budget_count;
        stats.snapshot_completion_wait_timeout_count =
            decode_stats->snapshot_pool.completion_wait_timeout_count;
        stats.exact_seek_candidate_cpu_bytes =
            decode_stats->exact_seek_candidate_cpu_bytes;
        stats.exact_seek_stable_cpu_bytes =
            decode_stats->exact_seek_stable_cpu_bytes;
        stats.exact_seek_reorder_count = decode_stats->exact_seek_reorder_count;
        stats.exact_seek_pending_count = decode_stats->exact_seek_pending_count;
        stats.exact_seek_stable_frame_count =
            decode_stats->exact_seek_stable_frame_count;
        stats.exact_seek_budget_drop_count =
            decode_stats->exact_seek_budget_drop_count;
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
        result.exact_seek_budget_drop_count += track.exact_seek_budget_drop_count;
        result.cpu_frame_bytes += track.total_cpu_frame_bytes;
        result.total_estimated_bytes +=
            track.decoder_pool_bytes + track.exact_seek_snapshot_bytes;
        result.tracks.push_back(std::move(track));
    }
    return result;
}

} // namespace vr
