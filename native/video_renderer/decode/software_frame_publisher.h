#pragma once

#include "video_renderer/buffer/track_buffer.h"

#include <optional>

struct AVFrame;

namespace vr {

std::optional<TextureFrame> make_bgra_texture_frame(const AVFrame* frame);

class SoftwareFrameQueuePublisher {
public:
    explicit SoftwareFrameQueuePublisher(TrackBuffer& output_buffer);

    bool publish_bgra_frame(const AVFrame* frame);

private:
    TrackBuffer& output_buffer_;
};

}  // namespace vr
