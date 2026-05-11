#include "video_renderer/track_pipeline.h"
#include "video_renderer/renderer_limits.h"

#include <spdlog/spdlog.h>

namespace vr {
namespace {
bool is_high_resolution_track(const DemuxStats& stats) {
    if (stats.width <= 0 || stats.height <= 0) {
        return false;
    }
    return static_cast<size_t>(stats.width) * static_cast<size_t>(stats.height) >=
        kHighResolutionTrackPixels;
}
}

DecodeDeviceMode default_decode_device_mode(AVCodecID codec_id) {
    if (codec_id == AV_CODEC_ID_AV1 || codec_id == AV_CODEC_ID_VP9) {
        return DecodeDeviceMode::FfmpegOwnedHwDownloadDevice;
    }
    return DecodeDeviceMode::IndependentDevice;
}

int TrackPipelineManager::find_empty_slot() const {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int TrackPipelineManager::find_slot_by_file_id(int file_id) const {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i] && tracks_[i]->file_id == file_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TrackPipelineManager::clear() {
    for (auto& track : tracks_) {
        track.reset();
    }
}

void TrackPipelineManager::stop_all(const TrackCallback& before_stop) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        stop_slot(i, before_stop);
    }
}

void TrackPipelineManager::stop_slot(size_t slot, const TrackCallback& before_stop) {
    if (slot >= kMaxTracks || !tracks_[slot]) {
        return;
    }
    auto& track = tracks_[slot];
    if (before_stop) {
        before_stop(slot, *track);
    }
    if (track->decode_thread) {
        spdlog::info("Renderer: stopping track[{}] decode ({})", slot, track->file_path);
        track->decode_thread->stop();
        spdlog::info("Renderer: track[{}] decode stopped", slot);
    }
    if (track->demux_thread) {
        spdlog::info("Renderer: stopping track[{}] demux ({})", slot, track->file_path);
        track->demux_thread->stop();
        spdlog::info("Renderer: track[{}] demux stopped", slot);
    }
    track.reset();
}

void TrackPipelineManager::compact_from(size_t slot, const MoveCallback& after_move) {
    if (slot >= kMaxTracks) {
        return;
    }
    for (size_t i = slot; i < kMaxTracks - 1; ++i) {
        if (!tracks_[i + 1]) {
            break;
        }
        tracks_[i] = std::move(tracks_[i + 1]);
        if (after_move) {
            after_move(i + 1, i, *tracks_[i]);
        }
    }
}

std::unique_ptr<TrackPipeline> TrackPipelineManager::create_pipeline(
    const std::string& path,
    bool hw_decode,
    const SeekRequest* initial_seek) const {
    auto pipeline = std::make_unique<TrackPipeline>();
    pipeline->file_path = path;
    pipeline->use_hardware_decode = hw_decode;
    pipeline->seek_controller = std::make_unique<SeekController>();
    if (initial_seek) {
        pipeline->seek_controller->request_seek(
            initial_seek->target_pts_us, initial_seek->type);
    }
    pipeline->packet_queue = std::make_unique<PacketQueue>(100);
    pipeline->audio_packet_queue = std::make_unique<PacketQueue>(100);

    pipeline->demux_thread = std::make_unique<DemuxThread>(
        path, *pipeline->seek_controller);
    pipeline->demux_thread->add_output(
        DemuxStreamKind::Video, *pipeline->packet_queue);
    pipeline->demux_thread->add_optional_output(
        DemuxStreamKind::Audio, *pipeline->audio_packet_queue);

    if (!pipeline->demux_thread->start()) {
        spdlog::error("Renderer: failed to start demux for {}", path);
        return nullptr;
    }

    const auto& stats = pipeline->demux_thread->stats();
    if (stats.video_stream_index < 0) {
        spdlog::error("Renderer: no video stream found in {}", path);
        pipeline->demux_thread->stop();
        return nullptr;
    }

    pipeline->video_width = stats.width;
    pipeline->video_height = stats.height;
    if (stats.width > 0 && stats.height > 0) {
        const float sar = (stats.sar_den > 0)
            ? static_cast<float>(stats.sar_num) / static_cast<float>(stats.sar_den)
            : 1.0f;
        pipeline->video_aspect =
            (static_cast<float>(stats.width) / static_cast<float>(stats.height)) * sar;
    }

    const size_t forward_depth =
        hw_decode && is_high_resolution_track(stats)
            ? kHighResolutionHardwareTrackForwardDepth
            : kDefaultTrackForwardDepth;
    pipeline->track_buffer = std::make_unique<TrackBuffer>(
        forward_depth, kDefaultTrackBackwardDepth);
    spdlog::info(
        "Renderer: track buffer depth forward={}, backward={}, max_cached={}, high_res={}, hw_decode={}",
        forward_depth,
        kDefaultTrackBackwardDepth,
        forward_depth + kDefaultTrackBackwardDepth,
        is_high_resolution_track(stats),
        hw_decode);

    pipeline->decode_thread = std::make_unique<DecodeThread>(
        *pipeline->packet_queue, *pipeline->track_buffer,
        stats.codec_params, stats.time_base);

    if (!pipeline->decode_thread->is_valid()) {
        spdlog::error("Renderer: decode thread init failed for {}", path);
        pipeline->demux_thread->stop();
        return nullptr;
    }

    pipeline->demux_thread->set_seek_callback(
        [dt = pipeline->decode_thread.get()](int64_t pts, SeekType type) {
            dt->notify_seek(pts, type);
        });

    if (hw_decode) {
        pipeline->decode_thread->enable_hardware_decode(
            default_decode_device_mode(stats.codec_params->codec_id));
    }

    if (!pipeline->decode_thread->start()) {
        spdlog::error("Renderer: failed to start decode for {}", path);
        pipeline->demux_thread->stop();
        return nullptr;
    }

    return pipeline;
}

} // namespace vr
