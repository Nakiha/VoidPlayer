#pragma once
#include "video_renderer/buffer/packet_queue.h"
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/decode/frame_converter.h"
#include "video_renderer/decode/hw/hw_decode_provider.h"
#include "video_renderer/sync/seek_controller.h"
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace vr {

class DecodeThread {
public:
    DecodeThread(PacketQueue& input_queue, TrackBuffer& output_buffer,
                 const AVCodecParameters* codec_params, AVRational time_base);
    ~DecodeThread();

    /// Returns true if the decoder was successfully initialized in the constructor.
    /// If false, start() will always fail — caller should not use this instance.
    bool is_valid() const { return codec_ctx_ != nullptr; }

    /// Enable hardware decode using the given native device.
    /// Must be called before start(). On failure, falls back to software.
    /// @param device_mutex  Shared mutex for D3D11 immediate context serialization.
    ///                      Must outlive this DecodeThread.
    bool enable_hardware_decode(void* native_device,
                                std::recursive_mutex* device_mutex = nullptr);

    bool start();
    void stop();

    /// Called from DemuxThread seek callback to notify this thread of a seek.
    void notify_seek(int64_t target_pts_us, SeekType type);

    /// Pause/resume packet processing. Set pause=true BEFORE requesting seek
    /// to prevent stale packets from being sent to the codec (avoids HEVC
    /// "Could not find ref" warnings during the seek transition).
    void set_decode_paused(bool paused);

private:
    void run();

    /// Attempt to open codec. Returns true on success.
    /// If hw_enabled_ is true and open fails, falls back to software.
    bool open_codec();

    PacketQueue& input_queue_;
    TrackBuffer& output_buffer_;
    FrameConverter converter_;

    AVCodecContext* codec_ctx_ = nullptr;
    const AVCodec* codec_ = nullptr;
    const AVCodecParameters* codec_params_;
    AVRational time_base_;

    // Hardware decode state
    void* native_device_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;   // Owned, from provider
    bool hw_enabled_ = false;
    HwDecodeType hw_type_ = HwDecodeType::None;
    std::unique_ptr<HwDecodeProvider> hw_provider_;  // Holds mutex lifetime
    AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;  // Per-instance, avoids global shared state

    // Seek coordination — protected by seek_mutex_ to avoid torn reads
    // between seek_target / seek_type / seek_pending
    std::mutex seek_mutex_;
    struct SeekState {
        bool pending = false;
        int64_t target_pts_us = 0;
        SeekType type = SeekType::Keyframe;
    };
    SeekState seek_;

    std::atomic<bool> decode_paused_{false};
    int64_t exact_seek_target_us_ = -1;  // >= 0 when discarding frames before exact seek target

    bool eof_flushed_ = false;
    bool post_seek_ = false;      // After seek: transition to Ready after 1 frame instead of full preroll

    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace vr
