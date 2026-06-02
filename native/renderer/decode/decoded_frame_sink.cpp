#include "renderer/decode/decoded_frame_sink.h"

#include <spdlog/spdlog.h>
#include <utility>

namespace vr {

DecodedFrameSink::~DecodedFrameSink() = default;

TrackBufferDecodedFrameSink::TrackBufferDecodedFrameSink(
    TrackBuffer& output_buffer,
    std::atomic<bool>& decode_paused,
    std::atomic<bool>& running)
    : output_buffer_(output_buffer)
    , decode_paused_(decode_paused)
    , running_(running) {}

void TrackBufferDecodedFrameSink::publish_decoded_frame(TextureFrame frame) {
    output_buffer_.push_frame(std::move(frame));
}

void TrackBufferDecodedFrameSink::fail_decoded_frame_publish(const char* context) {
    spdlog::error("[DecodeThread] Frame conversion failed ({})", context ? context : "unknown");
    output_buffer_.set_state(TrackState::Error);
    decode_paused_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

} // namespace vr
