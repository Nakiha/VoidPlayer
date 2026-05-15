#pragma once

#include "audio/audio_track_registry.h"
#include "media/packet_queue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

struct AVFrame;
struct SwrContext;

namespace vr {

class PcmBuffer;

class AudioDecodeThread final : public AudioTrackController {
public:
    AudioDecodeThread(PacketQueue& input_queue,
                      PcmBuffer& output_buffer,
                      const AVCodecParameters* codec_params,
                      AVRational time_base);
    ~AudioDecodeThread() override;

    bool start();
    void stop() override;
    void set_paused(bool paused) override;
    void notify_seek(int64_t target_pts_us, SeekType type) override;

private:
    bool init_resampler();
    void flush_after_seek_if_needed();
    void receive_frames(AVFrame* frame);
    void run();

    int64_t frame_pts_us(const AVFrame* frame) const;
    static int64_t frames_to_duration_us(size_t frames);

    PacketQueue& input_queue_;
    PcmBuffer& output_buffer_;
    const AVCodecParameters* codec_params_;
    AVRational time_base_;
    const AVCodec* codec_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwrContext* swr_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> decode_paused_{true};
    std::atomic<bool> seek_pending_{false};
};

} // namespace vr
