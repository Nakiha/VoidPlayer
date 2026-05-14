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

} // namespace vr
