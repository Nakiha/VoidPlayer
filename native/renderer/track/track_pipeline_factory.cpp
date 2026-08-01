#include "renderer/track/track_pipeline_factory.h"
#include "renderer/track/track_buffer_budget.h"

#include <spdlog/spdlog.h>

namespace vr {

DecodeDeviceMode default_decode_device_mode(
    AVCodecID codec_id,
    RenderBackendKind render_backend) {
    // Some Windows drivers can indefinitely block a D3D11 shared-snapshot
    // query when VP9 is added beside a high-resolution hardware track. Keep
    // D3D11VA decode, but download VP9 frames to deterministic CPU NV12 so
    // this codec never enters the cross-device snapshot path.
    if (render_backend == RenderBackendKind::NativeD3D11 &&
        codec_id == AV_CODEC_ID_VP9) {
        return DecodeDeviceMode::FfmpegOwnedHwDownloadDevice;
    }
    return DecodeDeviceMode::IndependentDevice;
}

std::unique_ptr<TrackPipeline> TrackPipelineFactory::create_opened_pipeline(
    const std::string& path,
    bool hw_decode,
    const SeekRequest* initial_seek,
    const TrackPipelineOpenOptions& options) const {
    auto pipeline = std::make_unique<TrackPipeline>();
    pipeline->file_path = path;
    pipeline->use_hardware_decode = hw_decode;
    pipeline->seek_controller = std::make_unique<SeekController>();
    if (initial_seek) {
        pipeline->seek_controller->request_seek(
            initial_seek->target_pts_us, initial_seek->type);
    }
    const auto budget = default_native_resource_budget();
    const size_t packet_queue_capacity =
        options.packet_queue_capacity > 0
            ? options.packet_queue_capacity
            : budget.packet_queue_capacity;
    pipeline->packet_queue =
        std::make_unique<PacketQueue>(packet_queue_capacity);
    pipeline->audio_packet_queue =
        std::make_unique<PacketQueue>(packet_queue_capacity);

    pipeline->demux_thread = std::make_unique<DemuxThread>(
        path, *pipeline->seek_controller);
    pipeline->demux_thread->add_output(
        DemuxStreamKind::Video, *pipeline->packet_queue);
    pipeline->demux_thread->add_optional_output(
        DemuxStreamKind::Audio, *pipeline->audio_packet_queue);

    if (!pipeline->demux_thread->open()) {
        spdlog::error("Renderer: failed to open demux for {}", path);
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
    pipeline->duration_us = stats.duration_us;
    if (stats.width > 0 && stats.height > 0) {
        const float sar = (stats.sar_den > 0)
            ? static_cast<float>(stats.sar_num) / static_cast<float>(stats.sar_den)
            : 1.0f;
        pipeline->video_aspect =
            (static_cast<float>(stats.width) / static_cast<float>(stats.height)) * sar;
    }

    const auto buffer_budget = choose_track_buffer_budget(stats, hw_decode, budget);
    pipeline->track_buffer = std::make_shared<TrackBuffer>(
        buffer_budget.forward_depth, buffer_budget.backward_depth);
    spdlog::info(
        "Renderer: track buffer depth forward={}, backward={}, max_cached={}, high_res={}, hw_decode={}",
        buffer_budget.forward_depth,
        buffer_budget.backward_depth,
        buffer_budget.max_cached_frames(),
        buffer_budget.high_resolution,
        hw_decode);

    pipeline->decode_thread = std::make_unique<DecodeThread>(
        *pipeline->packet_queue, *pipeline->track_buffer,
        stats.codec_params, stats.time_base);

    if (!pipeline->decode_thread->is_valid()) {
        spdlog::error("Renderer: decode thread init failed for {}", path);
        pipeline->demux_thread->stop();
        return nullptr;
    }

    if (hw_decode) {
        const auto decode_device_mode =
            options.use_default_decode_device_mode
                ? default_decode_device_mode(
                      stats.codec_params->codec_id, options.render_backend)
                : options.decode_device_mode;
        pipeline->decode_thread->enable_hardware_decode(
            decode_device_mode,
            options.render_device,
            options.device_mutex,
            options.render_backend);
    }

    if (!pipeline->decode_thread->start()) {
        spdlog::error("Renderer: failed to start decode for {}", path);
        pipeline->demux_thread->stop();
        return nullptr;
    }

    return pipeline;
}

} // namespace vr
