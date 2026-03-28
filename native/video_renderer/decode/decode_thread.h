#pragma once
#include "video_renderer/buffer/packet_queue.h"
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/decode/frame_converter.h"
#include "video_renderer/decode/hw/hw_decode_provider.h"
#include <thread>
#include <atomic>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace vr {

class DecodeThread {
public:
    DecodeThread(PacketQueue& input_queue, TrackBuffer& output_buffer,
                 const AVCodecParameters* codec_params, AVRational time_base);
    ~DecodeThread();

    /// Enable hardware decode using the given native device.
    /// Must be called before start(). On failure, falls back to software.
    /// @param device_mutex  Shared mutex for D3D11 immediate context serialization.
    ///                      Must outlive this DecodeThread.
    bool enable_hardware_decode(void* native_device,
                                std::recursive_mutex* device_mutex = nullptr);

    bool start();
    void stop();

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

    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace vr
