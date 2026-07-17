#include "renderer/decode/decoded_frame_publisher.h"

#include <chrono>
#include <utility>

namespace vr {
namespace {

uint64_t elapsed_us_since(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}

} // namespace

DecodedFramePublisher::DecodedFramePublisher(TrackBuffer& output_buffer,
                                             FrameConverter& converter,
                                             bool& hw_enabled,
                                             std::unique_ptr<HwDecodeProvider>& hw_provider,
                                             bool& hw_visibility_flush_pending,
                                             std::atomic<bool>& decode_paused,
                                             std::atomic<bool>& running,
                                             DecodeStagePerfCounters* stage_perf)
    : owned_sink_(std::make_unique<TrackBufferDecodedFrameSink>(
          output_buffer,
          decode_paused,
          running,
          stage_perf))
    , sink_(*owned_sink_)
    , converter_(converter)
    , hw_enabled_(hw_enabled)
    , hw_provider_(hw_provider)
    , hw_visibility_flush_pending_(hw_visibility_flush_pending)
    , stage_perf_(stage_perf) {}

DecodedFramePublisher::DecodedFramePublisher(DecodedFrameSink& sink,
                                             FrameConverter& converter,
                                             bool& hw_enabled,
                                             std::unique_ptr<HwDecodeProvider>& hw_provider,
                                             bool& hw_visibility_flush_pending,
                                             DecodeStagePerfCounters* stage_perf)
    : sink_(sink)
    , converter_(converter)
    , hw_enabled_(hw_enabled)
    , hw_provider_(hw_provider)
    , hw_visibility_flush_pending_(hw_visibility_flush_pending)
    , stage_perf_(stage_perf) {}

void DecodedFramePublisher::flush_visibility_if_needed() {
    if (!hw_enabled_ || !hw_provider_ || !hw_visibility_flush_pending_) {
        return;
    }
    if (converter_.hardware_snapshot_submits_shared_visibility()) {
        hw_visibility_flush_pending_ = false;
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    hw_provider_->flush();
    if (stage_perf_) {
        stage_perf_->record_flush(elapsed_us_since(start));
    }
    hw_visibility_flush_pending_ = false;
}

void DecodedFramePublisher::flush_before_publish_if_needed(bool force_for_shared_surface) {
    if (!hw_enabled_ || !hw_provider_) {
        return;
    }
    if (converter_.hardware_snapshot_submits_shared_visibility()) {
        hw_visibility_flush_pending_ = false;
        return;
    }
    if (!force_for_shared_surface && !converter_.downloads_hardware_to_cpu()) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    hw_provider_->flush();
    if (stage_perf_) {
        stage_perf_->record_flush(elapsed_us_since(start));
    }
    hw_visibility_flush_pending_ = false;
}

std::optional<TextureFrame> DecodedFramePublisher::convert_frame_for_publish(AVFrame* frame) {
    const auto start = std::chrono::steady_clock::now();
    std::optional<TextureFrame> converted;
    if (hw_enabled_ && !converter_.downloads_hardware_to_cpu()) {
        converted = converter_.snapshot_hardware_frame(frame);
    } else {
        converted = converter_.convert(frame, stage_perf_);
    }
    if (stage_perf_) {
        stage_perf_->record_convert(elapsed_us_since(start));
    }
    return converted;
}

bool DecodedFramePublisher::push_converted_frame(std::optional<TextureFrame> frame,
                                                 const char* context) {
    if (!frame.has_value()) {
        sink_.fail_decoded_frame_publish(context);
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    sink_.publish_decoded_frame(std::move(*frame));
    if (stage_perf_) {
        stage_perf_->record_publish(elapsed_us_since(start));
    }
    return true;
}

bool DecodedFramePublisher::convert_and_push_frame(AVFrame* frame, const char* context) {
    return push_converted_frame(convert_frame_for_publish(frame), context);
}

} // namespace vr
