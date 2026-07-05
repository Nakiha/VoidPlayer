#pragma once

#include "renderer/decode/decoded_frame_sink.h"
#include "renderer/decode/decode_stage_perf.h"
#include "renderer/decode/frame_converter.h"
#include "renderer/decode/hw/hw_decode_provider.h"

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
                          std::atomic<bool>& running,
                          DecodeStagePerfCounters* stage_perf = nullptr);
    DecodedFramePublisher(DecodedFrameSink& sink,
                          FrameConverter& converter,
                          bool& hw_enabled,
                          std::unique_ptr<HwDecodeProvider>& hw_provider,
                          bool& hw_visibility_flush_pending,
                          DecodeStagePerfCounters* stage_perf = nullptr);

    void flush_visibility_if_needed();
    void flush_before_publish_if_needed(bool force_for_shared_surface = false);
    std::optional<TextureFrame> convert_frame_for_publish(AVFrame* frame);
    bool push_converted_frame(std::optional<TextureFrame> frame, const char* context);
    bool convert_and_push_frame(AVFrame* frame, const char* context);

private:
    std::unique_ptr<TrackBufferDecodedFrameSink> owned_sink_;
    DecodedFrameSink& sink_;
    FrameConverter& converter_;
    bool& hw_enabled_;
    std::unique_ptr<HwDecodeProvider>& hw_provider_;
    bool& hw_visibility_flush_pending_;
    DecodeStagePerfCounters* stage_perf_ = nullptr;
};

} // namespace vr
