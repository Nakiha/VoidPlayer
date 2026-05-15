#pragma once

#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/decode/frame_converter.h"
#include "video_renderer/decode/hw/hw_decode_provider.h"

#include <atomic>
#include <memory>
#include <optional>

struct AVFrame;

namespace vr {

class DecodedFramePublisher {
public:
    DecodedFramePublisher(TrackBuffer& output_buffer,
                          FrameConverter& converter,
                          bool& hw_enabled,
                          std::unique_ptr<HwDecodeProvider>& hw_provider,
                          bool& hw_visibility_flush_pending,
                          std::atomic<bool>& decode_paused,
                          std::atomic<bool>& running);

    void flush_visibility_if_needed();
    void flush_before_publish_if_needed(bool force_for_shared_surface = false);
    std::optional<TextureFrame> convert_frame_for_publish(AVFrame* frame);
    bool push_converted_frame(std::optional<TextureFrame> frame, const char* context);
    bool convert_and_push_frame(AVFrame* frame, const char* context);

private:
    TrackBuffer& output_buffer_;
    FrameConverter& converter_;
    bool& hw_enabled_;
    std::unique_ptr<HwDecodeProvider>& hw_provider_;
    bool& hw_visibility_flush_pending_;
    std::atomic<bool>& decode_paused_;
    std::atomic<bool>& running_;
};

} // namespace vr
