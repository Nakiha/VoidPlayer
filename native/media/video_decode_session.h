#pragma once

#include "media/ffmpeg_lifetime.h"
#include "renderer/decode/codec_loop.h"
#include "renderer/decode/hw/hw_decode_provider.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

struct AVCodec;
struct AVCodecContext;
struct AVCodecParameters;
struct AVFrame;
struct AVPacket;

namespace vr {

struct VideoDecodeSessionOptions {
    int thread_count = 0;
    int export_side_data = 0;
    bool copy_packet_opaque = true;
};

/// Codec and hardware-device lifetime shared by realtime playback and offline
/// analysis. This class intentionally has no packet queue, seek, buffering, or
/// presentation policy.
class VideoDecodeSession {
public:
    VideoDecodeSession() = default;
    ~VideoDecodeSession();

    VideoDecodeSession(const VideoDecodeSession&) = delete;
    VideoDecodeSession& operator=(const VideoDecodeSession&) = delete;

    bool initialize(const AVCodecParameters* codec_params,
                    const VideoDecodeSessionOptions& options,
                    std::string& error);

    /// Configure a hardware decoder before open(). Failure leaves the session
    /// ready for software decoding.
    bool enable_hardware_decode(
        DecodeDeviceMode mode = DecodeDeviceMode::IndependentDevice,
        void* render_device = nullptr,
        std::recursive_mutex* device_mutex = nullptr,
        RenderBackendKind backend = default_render_backend_kind(),
        std::string* diagnostic = nullptr);

    /// Open the configured decoder. Hardware-open failures are retried with
    /// the preferred software decoder.
    bool open(std::string& error);

    int send_packet(const AVPacket* packet);
    int receive_frame(AVFrame* frame);
    void flush();
    void close();

    bool is_valid() const noexcept;
    bool is_open() const noexcept { return opened_; }
    bool hardware_enabled() const noexcept { return hw_enabled_; }
    bool hardware_output_downloads_to_cpu() const noexcept;
    bool hardware_surfaces_are_renderer_owned() const noexcept;

    AVCodecContext* codec_context() const noexcept { return codec_ctx_.get(); }
    const AVCodec* codec() const noexcept { return codec_; }
    const AVCodecParameters* codec_parameters() const noexcept {
        return codec_params_;
    }
    HwDecodeType hardware_type() const noexcept { return hw_type_; }
    HwDecodeProvider* hardware_provider() const noexcept {
        return hw_provider_.get();
    }
    bool& hardware_enabled_flag() noexcept { return hw_enabled_; }
    std::unique_ptr<HwDecodeProvider>& hardware_provider_owner() noexcept {
        return hw_provider_;
    }
    DecodeDeviceMode decode_device_mode() const noexcept {
        return decode_device_mode_;
    }
    void* native_device() const noexcept { return native_device_; }
    std::recursive_mutex* device_mutex() const noexcept {
        return device_mutex_;
    }

    void set_codec_open_for_test(CodecOpenFunction open_fn) noexcept {
        codec_open_for_test_ = open_fn;
    }

private:
    bool reset_codec_context(const AVCodec* codec, std::string& error);
    void apply_software_codec_policy();
    const AVCodec* preferred_software_decoder() const;
    void clear_hardware_state();
    static AVPixelFormat choose_hardware_format(
        AVCodecContext* context,
        const AVPixelFormat* formats);

    AVCodecParameters* codec_params_ = nullptr;
    VideoDecodeSessionOptions options_;
    AvCodecContextOwner codec_ctx_;
    const AVCodec* codec_ = nullptr;
    const AVCodec* primary_decoder_ = nullptr;
    CodecOpenFunction codec_open_for_test_ = nullptr;

    void* native_device_ = nullptr;
    DecodeDeviceMode decode_device_mode_ = DecodeDeviceMode::IndependentDevice;
    AvBufferRefOwner hw_device_ctx_;
    bool hw_enabled_ = false;
    HwDecodeType hw_type_ = HwDecodeType::None;
    std::unique_ptr<HwDecodeProvider> hw_provider_;
    AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;
    std::recursive_mutex* device_mutex_ = nullptr;
    bool opened_ = false;
};

}  // namespace vr
