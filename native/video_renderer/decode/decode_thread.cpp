#include "video_renderer/decode/decode_thread.h"
#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace vr {

// get_format callback for hardware decode negotiation.
// Reads the preferred hw pixel format from opaque pointer (per-instance).
static enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                         const enum AVPixelFormat* pix_fmts) {
    auto* preferred = static_cast<AVPixelFormat*>(ctx->opaque);
    // Prefer D3D11VA_VLD over D3D11 when both are available.
    AVPixelFormat fallback = AV_PIX_FMT_NONE;
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (preferred && *p == *preferred) {
            return *p;
        }
        if (*p == AV_PIX_FMT_D3D11VA_VLD) {
            fallback = *p;
        }
    }
    if (fallback != AV_PIX_FMT_NONE) {
        spdlog::info("[DecodeThread] get_format: using D3D11VA_VLD fallback");
        return fallback;
    }
    spdlog::warn("[DecodeThread] HW pixel format not available in get_format, returning NONE");
    return AV_PIX_FMT_NONE;
}

DecodeThread::DecodeThread(PacketQueue& input_queue, TrackBuffer& output_buffer,
                           const AVCodecParameters* codec_params, AVRational time_base)
    : input_queue_(input_queue)
    , output_buffer_(output_buffer)
    , codec_params_(codec_params)
    , time_base_(time_base)
{
    // Find decoder
    codec_ = avcodec_find_decoder(codec_params->codec_id);
    if (!codec_) {
        spdlog::error("[DecodeThread] No decoder found for codec_id={}",
                      static_cast<int>(codec_params->codec_id));
        return;
    }

    spdlog::info("[DecodeThread] Using decoder: {}", codec_->name);

    // Allocate codec context
    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_) {
        spdlog::error("[DecodeThread] Failed to allocate codec context");
        return;
    }

    // Copy codec parameters to context
    int ret = avcodec_parameters_to_context(codec_ctx_, codec_params);
    if (ret < 0) {
        spdlog::error("[DecodeThread] Failed to copy codec parameters: {}", ret);
        avcodec_free_context(&codec_ctx_);
        return;
    }

    // NOTE: avcodec_open2 is NOT called here.
    // It is deferred to start() so that enable_hardware_decode() can set
    // hw_device_ctx before the codec is opened.
}

DecodeThread::~DecodeThread() {
    stop();
}

bool DecodeThread::enable_hardware_decode(void* native_device,
                                           std::recursive_mutex* device_mutex) {
    if (!codec_ctx_ || !codec_) {
        spdlog::warn("[DecodeThread] Cannot enable hw decode: codec not initialized");
        return false;
    }

    native_device_ = native_device;

    auto result = try_hw_decode_providers(
        native_device, codec_, codec_params_->width, codec_params_->height,
        device_mutex);

    if (!result.success) {
        spdlog::info("[DecodeThread] Hardware decode not available, will use software");
        hw_enabled_ = false;
        return false;
    }

    // Store hw device context and provider
    hw_device_ctx_ = result.hw_device_ctx;
    hw_type_ = result.type;
    hw_enabled_ = true;
    hw_pix_fmt_ = result.hw_pix_fmt;

    // Set hw_device_ctx on codec context BEFORE opening
    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);

    // Increase the hw frame pool size to accommodate both the decoder's
    // reference frame pool (DPB) and our preroll/render pipeline buffers.
    // Without this, the default pool (20) is too small when preroll frames
    // hold references via av_frame_ref while the decoder also needs surfaces.
    codec_ctx_->extra_hw_frames = 32;

    // NOTE: We intentionally do NOT create hw_frames_ctx here.
    // FFmpeg's internal ff_decode_get_hw_frames_ctx() will create one
    // automatically when the codec is opened with hw_device_ctx set.
    // This avoids configuration mismatches that caused 0-frame output.

    // Set get_format callback with per-instance opaque pointer
    codec_ctx_->get_format = get_hw_format;
    codec_ctx_->opaque = &hw_pix_fmt_;

    spdlog::info("[DecodeThread] Hardware decode enabled via {} (pix_fmt={})",
                 result.type == HwDecodeType::D3D11VA ? "D3D11VA" : "unknown",
                 static_cast<int>(result.hw_pix_fmt));
    return true;
}

bool DecodeThread::open_codec() {
    int ret = avcodec_open2(codec_ctx_, codec_, nullptr);
    if (ret == 0) return true;

    spdlog::error("[DecodeThread] Failed to open codec: {}", ret);

    // If HW decode was attempted, try fallback to software
    if (hw_enabled_) {
        spdlog::info("[DecodeThread] Attempting software fallback...");

        // Clean up hw state
        if (codec_ctx_->hw_device_ctx) {
            av_buffer_unref(&codec_ctx_->hw_device_ctx);
        }
        hw_enabled_ = false;
        hw_pix_fmt_ = AV_PIX_FMT_NONE;

        // Recreate codec context without hw
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = avcodec_alloc_context3(codec_);
        if (!codec_ctx_) {
            spdlog::error("[DecodeThread] Failed to allocate sw codec context");
            return false;
        }

        int ret2 = avcodec_parameters_to_context(codec_ctx_, codec_params_);
        if (ret2 < 0) {
            spdlog::error("[DecodeThread] Failed to copy params for sw fallback");
            avcodec_free_context(&codec_ctx_);
            return false;
        }

        ret2 = avcodec_open2(codec_ctx_, codec_, nullptr);
        if (ret2 < 0) {
            spdlog::error("[DecodeThread] Software fallback also failed: {}", ret2);
            return false;
        }

        spdlog::info("[DecodeThread] Software fallback succeeded");
        return true;
    }

    return false;
}

bool DecodeThread::start() {
    if (!codec_ctx_) {
        spdlog::error("[DecodeThread] Cannot start: codec not initialized");
        return false;
    }
    if (running_.load()) return false;

    // Open codec (deferred from constructor so hw_device_ctx can be set first)
    if (!open_codec()) {
        spdlog::error("[DecodeThread] Cannot start: codec open failed");
        return false;
    }

    spdlog::info("[DecodeThread] Codec opened successfully ({}x{})",
                 codec_ctx_->width, codec_ctx_->height);

    // Initialize the frame converter based on decode mode
    bool conv_ok;
    if (hw_enabled_) {
        conv_ok = converter_.init_hardware(native_device_, nullptr,
                                           codec_ctx_->width, codec_ctx_->height,
                                           hw_type_);
    } else {
        conv_ok = converter_.init_software(codec_ctx_->width, codec_ctx_->height,
                                           codec_ctx_->pix_fmt);
    }
    if (!conv_ok) {
        spdlog::error("[DecodeThread] Failed to initialize frame converter");
        return false;
    }

    output_buffer_.set_state(TrackState::Buffering);
    running_.store(true);
    thread_ = std::thread(&DecodeThread::run, this);
    return true;
}

void DecodeThread::stop() {
    running_.store(false);
    input_queue_.abort();   // Unblock blocking pop
    output_buffer_.abort(); // Unblock blocking push_frame
    if (thread_.joinable()) {
        thread_.join();
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
    hw_provider_.reset();
}

void DecodeThread::run() {
    spdlog::info("[DecodeThread] Decode loop started (hw={})", hw_enabled_);

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        spdlog::error("[DecodeThread] Failed to allocate frame");
        output_buffer_.set_state(TrackState::Error);
        return;
    }

    while (running_.load()) {
        AVPacket* pkt = input_queue_.pop();
        if (!pkt) {
            spdlog::info("[DecodeThread] Input queue aborted");
            break;
        }

        int ret = avcodec_send_packet(codec_ctx_, pkt);
        av_packet_free(&pkt);

        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            spdlog::error("[DecodeThread] Error sending packet: {}", ret);
            continue;
        }

        while (true) {
            ret = avcodec_receive_frame(codec_ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                spdlog::error("[DecodeThread] Error receiving frame: {}", ret);
                break;
            }

            // Rescale timestamps from stream time_base to microseconds
            if (frame->pts != AV_NOPTS_VALUE) {
                frame->pts = av_rescale_q(frame->pts, time_base_, {1, 1000000});
            } else if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                frame->pts = av_rescale_q(frame->best_effort_timestamp, time_base_, {1, 1000000});
            }
            if (frame->duration > 0 && frame->duration != AV_NOPTS_VALUE) {
                frame->duration = av_rescale_q(frame->duration, time_base_, {1, 1000000});
            }

            TextureFrame tex_frame = converter_.convert(frame);

            output_buffer_.push_frame(std::move(tex_frame));

            if (output_buffer_.state() == TrackState::Buffering &&
                output_buffer_.has_preroll()) {
                spdlog::debug("[DecodeThread] Preroll complete ({} frames buffered)",
                             output_buffer_.total_count());
                output_buffer_.set_state(TrackState::Ready);
            }

            av_frame_unref(frame);
        }
    }

    // Flush the decoder
    avcodec_send_packet(codec_ctx_, nullptr);
    while (true) {
        int ret = avcodec_receive_frame(codec_ctx_, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        if (frame->pts != AV_NOPTS_VALUE) {
            frame->pts = av_rescale_q(frame->pts, time_base_, {1, 1000000});
        }
        if (frame->duration > 0 && frame->duration != AV_NOPTS_VALUE) {
            frame->duration = av_rescale_q(frame->duration, time_base_, {1, 1000000});
        }

        TextureFrame tex_frame = converter_.convert(frame);
        output_buffer_.push_frame(std::move(tex_frame));
        av_frame_unref(frame);
    }

    output_buffer_.set_state(TrackState::Flushing);
    av_frame_free(&frame);
    spdlog::info("[DecodeThread] Decode loop ended");
}

} // namespace vr
