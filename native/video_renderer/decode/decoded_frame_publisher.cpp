#include "video_renderer/decode/decoded_frame_publisher.h"

#include <utility>

namespace vr {

DecodedFramePublisher::DecodedFramePublisher(TrackBuffer& output_buffer,
                                             FrameConverter& converter,
                                             bool& hw_enabled,
                                             std::unique_ptr<HwDecodeProvider>& hw_provider,
                                             bool& hw_visibility_flush_pending,
                                             std::atomic<bool>& decode_paused,
                                             std::atomic<bool>& running)
    : owned_sink_(std::make_unique<TrackBufferDecodedFrameSink>(
          output_buffer,
          decode_paused,
          running))
    , sink_(*owned_sink_)
    , converter_(converter)
    , hw_enabled_(hw_enabled)
    , hw_provider_(hw_provider)
    , hw_visibility_flush_pending_(hw_visibility_flush_pending) {}

DecodedFramePublisher::DecodedFramePublisher(DecodedFrameSink& sink,
                                             FrameConverter& converter,
                                             bool& hw_enabled,
                                             std::unique_ptr<HwDecodeProvider>& hw_provider,
                                             bool& hw_visibility_flush_pending)
    : sink_(sink)
    , converter_(converter)
    , hw_enabled_(hw_enabled)
    , hw_provider_(hw_provider)
    , hw_visibility_flush_pending_(hw_visibility_flush_pending) {}

void DecodedFramePublisher::flush_visibility_if_needed() {
    if (!hw_enabled_ || !hw_provider_ || !hw_visibility_flush_pending_) {
        return;
    }
    hw_provider_->flush();
    hw_visibility_flush_pending_ = false;
}

void DecodedFramePublisher::flush_before_publish_if_needed(bool force_for_shared_surface) {
    if (!hw_enabled_ || !hw_provider_) {
        return;
    }
    if (!force_for_shared_surface && !converter_.downloads_hardware_to_cpu()) {
        return;
    }
    hw_provider_->flush();
    hw_visibility_flush_pending_ = false;
}

std::optional<TextureFrame> DecodedFramePublisher::convert_frame_for_publish(AVFrame* frame) {
    if (hw_enabled_ && !converter_.downloads_hardware_to_cpu()) {
        return converter_.snapshot_hardware_frame(frame);
    }
    return converter_.convert(frame);
}

bool DecodedFramePublisher::push_converted_frame(std::optional<TextureFrame> frame,
                                                 const char* context) {
    if (!frame.has_value()) {
        sink_.fail_decoded_frame_publish(context);
        return false;
    }
    sink_.publish_decoded_frame(std::move(*frame));
    return true;
}

bool DecodedFramePublisher::convert_and_push_frame(AVFrame* frame, const char* context) {
    return push_converted_frame(convert_frame_for_publish(frame), context);
}

} // namespace vr
