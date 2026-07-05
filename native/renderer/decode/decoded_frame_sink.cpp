#include "renderer/decode/decoded_frame_sink.h"
#include "renderer/decode/decode_stage_perf.h"

#include <spdlog/spdlog.h>
#include <utility>

namespace vr {

DecodedFrameSink::~DecodedFrameSink() = default;

TrackBufferDecodedFrameSink::TrackBufferDecodedFrameSink(
    TrackBuffer& output_buffer,
    std::atomic<bool>& decode_paused,
    std::atomic<bool>& running,
    DecodeStagePerfCounters* stage_perf)
    : output_buffer_(output_buffer)
    , decode_paused_(decode_paused)
    , running_(running)
    , stage_perf_(stage_perf) {}

void TrackBufferDecodedFrameSink::publish_decoded_frame(TextureFrame frame) {
    TrackBufferPushTiming timing;
    output_buffer_.push_frame(std::move(frame), &timing);
    if (stage_perf_) {
        stage_perf_->record_publish_lock(timing.lock_us);
        stage_perf_->record_publish_wait(timing.wait_us);
        stage_perf_->record_publish_ring_push(timing.push_us);
        stage_perf_->record_publish_ring_lock(timing.ring_lock_us);
        stage_perf_->record_publish_ring_assign(timing.ring_assign_us);
        stage_perf_->record_publish_ring_advance(timing.ring_advance_us);
        stage_perf_->record_publish_ring_overwrite_bytes(
            timing.ring_overwritten_cpu_bytes);
    }
}

void TrackBufferDecodedFrameSink::fail_decoded_frame_publish(const char* context) {
    spdlog::error("[DecodeThread] Frame conversion failed ({})", context ? context : "unknown");
    output_buffer_.set_state(TrackState::Error);
    decode_paused_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
}

} // namespace vr
