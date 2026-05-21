#include "video_renderer/decode/software_frame_publisher.h"

#include "video_renderer/decode/software_bgra_converter.h"

#include <memory>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

namespace vr {

std::optional<TextureFrame> make_bgra_texture_frame(const AVFrame* frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0) {
        return std::nullopt;
    }

    const size_t row_bytes = static_cast<size_t>(frame->width) * 4;
    const size_t bgra_size = row_bytes * static_cast<size_t>(frame->height);
    auto data = std::make_shared<std::vector<uint8_t>>(bgra_size);
    if (!convert_software_frame_to_bgra(frame, data->data(), data->size())) {
        return std::nullopt;
    }

    TextureFrame result;
    result.pts_us = frame->pts == AV_NOPTS_VALUE ? 0 : frame->pts;
    result.dts_us = frame->pkt_dts == AV_NOPTS_VALUE ? kNoTimestampUs : frame->pkt_dts;
    result.duration_us =
        frame->duration > 0 && frame->duration != AV_NOPTS_VALUE ? frame->duration : 0;
    result.width = frame->width;
    result.height = frame->height;
    result.cpu_data = data;
    result.texture_handle = data->data();
    result.storage = CpuRgbaFrameStorage{
        data,
        static_cast<int>(row_bytes),
    };
    return result;
}

SoftwareFrameQueuePublisher::SoftwareFrameQueuePublisher(TrackBuffer& output_buffer)
    : output_buffer_(output_buffer) {}

bool SoftwareFrameQueuePublisher::publish_bgra_frame(const AVFrame* frame) {
    auto texture_frame = make_bgra_texture_frame(frame);
    if (!texture_frame) {
        return false;
    }
    output_buffer_.push_frame(std::move(*texture_frame));
    return true;
}

}  // namespace vr
