#pragma once

#include "renderer/buffer/track_buffer.h"

#include <atomic>

namespace vr {

struct DecodeStagePerfCounters;

class DecodedFrameSink {
public:
    virtual ~DecodedFrameSink();

    virtual void publish_decoded_frame(TextureFrame frame) = 0;
    virtual void fail_decoded_frame_publish(const char* context) = 0;
};

class TrackBufferDecodedFrameSink final : public DecodedFrameSink {
public:
    TrackBufferDecodedFrameSink(TrackBuffer& output_buffer,
                                std::atomic<bool>& decode_paused,
                                std::atomic<bool>& running,
                                DecodeStagePerfCounters* stage_perf = nullptr);

    void publish_decoded_frame(TextureFrame frame) override;
    void fail_decoded_frame_publish(const char* context) override;

private:
    TrackBuffer& output_buffer_;
    std::atomic<bool>& decode_paused_;
    std::atomic<bool>& running_;
    DecodeStagePerfCounters* stage_perf_ = nullptr;
};

} // namespace vr
