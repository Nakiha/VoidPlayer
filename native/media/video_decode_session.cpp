#include "media/video_decode_session.h"

#include <algorithm>
#include <limits>
#include <sstream>

#include <spdlog/spdlog.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

namespace vr {
namespace {

constexpr int kRendererOwnedHwExtraFrames = 0;

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

const char* decode_device_mode_name(DecodeDeviceMode mode) {
    switch (mode) {
    case DecodeDeviceMode::IndependentDevice:
        return "IndependentDevice";
    case DecodeDeviceMode::SharedRenderDevice:
        return "SharedRenderDevice";
    case DecodeDeviceMode::FfmpegOwnedHwDownloadDevice:
        return "FfmpegOwnedHwDownloadDevice";
    }
    return "Unknown";
}

}  // namespace

VideoDecodeSession::~VideoDecodeSession() {
    close();
}

bool VideoDecodeSession::initialize(
    const AVCodecParameters* codec_params,
    const VideoDecodeSessionOptions& options,
    std::string& error) {
    close();
    error.clear();
    if (!codec_params) {
        error = "codec parameters are null";
        return false;
    }

    codec_params_ = avcodec_parameters_alloc();
    if (!codec_params_) {
        error = "avcodec_parameters_alloc failed";
        return false;
    }
    const int copy_result =
        avcodec_parameters_copy(codec_params_, codec_params);
    if (copy_result < 0) {
        error = "avcodec_parameters_copy failed: " +
                ffmpeg_error(copy_result);
        close();
        return false;
    }
    options_ = options;

    if (codec_params_->codec_id == AV_CODEC_ID_AV1) {
        primary_decoder_ = avcodec_find_decoder_by_name("av1");
        if (primary_decoder_) {
            spdlog::info(
                "[VideoDecodeSession] AV1 native decoder selected first "
                "for hardware negotiation");
        }
    }
    if (!primary_decoder_) {
        primary_decoder_ =
            avcodec_find_decoder(codec_params_->codec_id);
    }
    if (!primary_decoder_) {
        error = "no decoder found for codec " +
                std::string(avcodec_get_name(codec_params_->codec_id));
        close();
        return false;
    }

    if (!reset_codec_context(primary_decoder_, error)) {
        close();
        return false;
    }
    spdlog::debug("[VideoDecodeSession] decoder prepared: {}",
                  codec_->name ? codec_->name : "unknown");
    return true;
}

bool VideoDecodeSession::enable_hardware_decode(
    DecodeDeviceMode mode,
    void* render_device,
    std::recursive_mutex* device_mutex,
    RenderBackendKind backend,
    std::string* diagnostic) {
    auto set_diagnostic = [&](const std::string& message) {
        if (diagnostic) {
            *diagnostic = message;
        }
    };

    if (!codec_ctx_ || !codec_ || !codec_params_) {
        set_diagnostic("codec session is not initialized");
        return false;
    }
    if (opened_) {
        set_diagnostic("hardware decode must be configured before open");
        return false;
    }
    if (mode == DecodeDeviceMode::SharedRenderDevice &&
        !render_device) {
        set_diagnostic(
            "SharedRenderDevice requires a render device");
        return false;
    }

    decode_device_mode_ = mode;
    native_device_ =
        mode == DecodeDeviceMode::SharedRenderDevice
            ? render_device
            : nullptr;
    device_mutex_ = device_mutex;

    HwDecodeInitParams params;
    params.backend = backend;
    params.device_mode = mode;
    params.render_device = render_device;
    params.width = codec_params_->width;
    params.height = codec_params_->height;
    params.device_mutex = device_mutex;

    spdlog::debug(
        "[VideoDecodeSession] hardware device mode: {}",
        decode_device_mode_name(mode));
    auto result = try_hw_decode_providers(codec_, params);
    if (!result.success) {
        set_diagnostic("no compatible hardware decoder available");
        const AVCodec* software = preferred_software_decoder();
        std::string reset_error;
        if (software && software != codec_ &&
            !reset_codec_context(software, reset_error)) {
            set_diagnostic(
                "hardware unavailable and software reset failed: " +
                reset_error);
        }
        return false;
    }

    hw_device_ctx_.reset(result.hw_device_ctx);
    hw_provider_ = std::move(result.provider);
    hw_type_ = result.type;
    hw_enabled_ = true;
    hw_pix_fmt_ = result.hw_pix_fmt;

    codec_ctx_->hw_device_ctx =
        av_buffer_ref(hw_device_ctx_.get());
    if (!codec_ctx_->hw_device_ctx) {
        set_diagnostic("failed to retain hardware device context");
        clear_hardware_state();
        std::string reset_error;
        reset_codec_context(preferred_software_decoder(), reset_error);
        return false;
    }
    if (hardware_surfaces_are_renderer_owned()) {
        codec_ctx_->extra_hw_frames =
            kRendererOwnedHwExtraFrames;
    }
    codec_ctx_->get_format = choose_hardware_format;
    codec_ctx_->opaque = &hw_pix_fmt_;

    set_diagnostic(
        std::string("enabled ") +
        hw_decode_type_name(hw_type_));
    spdlog::info(
        "[VideoDecodeSession] hardware decode enabled via {} "
        "(pix_fmt={})",
        hw_decode_type_name(hw_type_),
        static_cast<int>(hw_pix_fmt_));
    return true;
}

bool VideoDecodeSession::open(std::string& error) {
    error.clear();
    if (!codec_ctx_ || !codec_) {
        error = "codec session is not initialized";
        return false;
    }
    if (opened_) {
        return true;
    }

    if (!hw_enabled_) {
        const AVCodec* software = preferred_software_decoder();
        if (software && software != codec_ &&
            !reset_codec_context(software, error)) {
            return false;
        }
    }

    apply_software_codec_policy();
    int result = open_codec_seh_guarded(
        codec_ctx_.get(),
        codec_,
        nullptr,
        codec_open_for_test_);
    if (result >= 0) {
        opened_ = true;
        return true;
    }

    const std::string hardware_error =
        ffmpeg_error(result);
    if (!hw_enabled_) {
        error = "avcodec_open2 failed: " + hardware_error;
        return false;
    }

    spdlog::warn(
        "[VideoDecodeSession] hardware codec open failed ({}); "
        "retrying software",
        hardware_error);
    clear_hardware_state();
    const AVCodec* software = preferred_software_decoder();
    if (!reset_codec_context(
            software ? software : primary_decoder_, error)) {
        return false;
    }
    apply_software_codec_policy();
    result = open_codec_seh_guarded(
        codec_ctx_.get(),
        codec_,
        nullptr,
        codec_open_for_test_);
    if (result < 0) {
        error = "software fallback avcodec_open2 failed: " +
                ffmpeg_error(result);
        return false;
    }
    opened_ = true;
    spdlog::info(
        "[VideoDecodeSession] software fallback succeeded");
    return true;
}

int VideoDecodeSession::send_packet(const AVPacket* packet) {
    if (!opened_ || !codec_ctx_) {
        return AVERROR(EINVAL);
    }
    return send_codec_packet_seh_guarded(
        codec_ctx_.get(),
        packet,
        hw_enabled_,
        device_mutex_);
}

int VideoDecodeSession::receive_frame(AVFrame* frame) {
    if (!opened_ || !codec_ctx_ || !frame) {
        return AVERROR(EINVAL);
    }
    return receive_codec_frame_seh_guarded(
        codec_ctx_.get(),
        frame,
        hw_enabled_,
        device_mutex_);
}

void VideoDecodeSession::flush() {
    if (!codec_ctx_) {
        return;
    }
    avcodec_flush_buffers(codec_ctx_.get());
    if (hw_enabled_ && hw_provider_) {
        hw_provider_->flush();
    }
}

void VideoDecodeSession::close() {
    opened_ = false;
    codec_ctx_.reset();
    clear_hardware_state();
    codec_ = nullptr;
    primary_decoder_ = nullptr;
    if (codec_params_) {
        avcodec_parameters_free(&codec_params_);
    }
    options_ = {};
    codec_open_for_test_ = nullptr;
}

bool VideoDecodeSession::is_valid() const noexcept {
    return codec_ctx_.get() != nullptr &&
           codec_ != nullptr &&
           codec_params_ != nullptr;
}

bool VideoDecodeSession::hardware_output_downloads_to_cpu()
    const noexcept {
    return hw_enabled_ &&
           decode_device_mode_ ==
               DecodeDeviceMode::FfmpegOwnedHwDownloadDevice;
}

bool VideoDecodeSession::hardware_surfaces_are_renderer_owned()
    const noexcept {
    return hw_enabled_ &&
           !hardware_output_downloads_to_cpu();
}

bool VideoDecodeSession::reset_codec_context(
    const AVCodec* codec,
    std::string& error) {
    if (!codec || !codec_params_) {
        error = "cannot allocate codec context without decoder parameters";
        return false;
    }

    codec_ctx_.reset();
    codec_ = codec;
    codec_ctx_ = AvCodecContextOwner::allocate(codec_);
    if (!codec_ctx_) {
        error = "avcodec_alloc_context3 failed for " +
                std::string(codec_->name ? codec_->name : "unknown");
        return false;
    }
    const int result = avcodec_parameters_to_context(
        codec_ctx_.get(), codec_params_);
    if (result < 0) {
        error = "avcodec_parameters_to_context failed: " +
                ffmpeg_error(result);
        codec_ctx_.reset();
        return false;
    }

    codec_ctx_->thread_count =
        std::max(0, options_.thread_count);
    codec_ctx_->export_side_data |= options_.export_side_data;
#ifdef AV_CODEC_FLAG_COPY_OPAQUE
    if (options_.copy_packet_opaque) {
        codec_ctx_->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
    }
#endif
    return true;
}

void VideoDecodeSession::apply_software_codec_policy() {
    if (hw_enabled_ || !codec_ctx_ ||
        codec_ctx_->codec_id != AV_CODEC_ID_VVC) {
        return;
    }

    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    spdlog::info(
        "[VideoDecodeSession] VVC bounded frame-context policy enabled "
        "(low_delay=true, thread_count={})",
        codec_ctx_->thread_count);
}

const AVCodec* VideoDecodeSession::preferred_software_decoder()
    const {
    if (codec_params_ &&
        codec_params_->codec_id == AV_CODEC_ID_AV1) {
        const AVCodec* dav1d =
            avcodec_find_decoder_by_name("libdav1d");
        if (dav1d) {
            return dav1d;
        }
    }
    return primary_decoder_ ? primary_decoder_ : codec_;
}

void VideoDecodeSession::clear_hardware_state() {
    if (codec_ctx_ && codec_ctx_->hw_device_ctx) {
        av_buffer_unref(&codec_ctx_->hw_device_ctx);
    }
    hw_enabled_ = false;
    hw_type_ = HwDecodeType::None;
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
    hw_device_ctx_.reset();
    hw_provider_.reset();
    native_device_ = nullptr;
    device_mutex_ = nullptr;
    decode_device_mode_ = DecodeDeviceMode::IndependentDevice;
}

AVPixelFormat VideoDecodeSession::choose_hardware_format(
    AVCodecContext* context,
    const AVPixelFormat* formats) {
    if (!formats) {
        return AV_PIX_FMT_NONE;
    }
    auto* preferred =
        context
            ? static_cast<AVPixelFormat*>(context->opaque)
            : nullptr;
    AVPixelFormat fallback = AV_PIX_FMT_NONE;
    for (const AVPixelFormat* format = formats;
         *format != AV_PIX_FMT_NONE;
         ++format) {
        if (preferred && *format == *preferred) {
            return *format;
        }
#ifdef _WIN32
        if (*format == AV_PIX_FMT_D3D12 &&
            fallback == AV_PIX_FMT_NONE) {
            fallback = *format;
        }
#endif
    }
    if (fallback != AV_PIX_FMT_NONE) {
        const char* name = av_get_pix_fmt_name(fallback);
        spdlog::info(
            "[VideoDecodeSession] using hardware format fallback {}",
            name ? name : "unknown");
        return fallback;
    }
    spdlog::warn(
        "[VideoDecodeSession] requested hardware pixel format "
        "was not offered by the decoder");
    return AV_PIX_FMT_NONE;
}

}  // namespace vr
