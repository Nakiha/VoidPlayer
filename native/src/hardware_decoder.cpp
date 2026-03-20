#include "voidview_native/hardware_decoder.hpp"
#include "voidview_native/texture_interop.hpp"
#include "voidview_native/logger.hpp"

#include <algorithm>
#include <cstring>
#include <memory>

#ifdef _WIN32
#include <GL/gl.h>
#endif

// OpenGL constants (in case GL headers don't define them)
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#ifndef GL_TEXTURE_MIN_FILTER
#define GL_TEXTURE_MIN_FILTER 0x2801
#endif

#ifndef GL_TEXTURE_MAG_FILTER
#define GL_TEXTURE_MAG_FILTER 0x2800
#endif

#ifndef GL_TEXTURE_WRAP_S
#define GL_TEXTURE_WRAP_S 0x2802
#endif

#ifndef GL_TEXTURE_WRAP_T
#define GL_TEXTURE_WRAP_T 0x2803
#endif

#ifndef GL_LINEAR
#define GL_LINEAR 0x2601
#endif

#ifndef GL_RGBA
#define GL_RGBA 0x1908
#endif

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif

#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D 0x0DE1
#endif

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace voidview {

// ==================== HardwareDecoder::Impl ====================

class HardwareDecoder::Impl {
public:
    Impl() {
        frame_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
    }

    ~Impl() {
        cleanup();
        if (frame_) av_frame_free(&frame_);
        if (pkt_) av_packet_free(&pkt_);
    }

    bool open_source(const std::string& url) {
        int ret = avformat_open_input(&fmt_ctx_, url.c_str(), nullptr, nullptr);
        if (ret < 0) {
            set_error("Failed to open input", ret);
            return false;
        }

        ret = avformat_find_stream_info(fmt_ctx_, nullptr);
        if (ret < 0) {
            set_error("Failed to find stream info", ret);
            return false;
        }

        video_stream_idx_ = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (video_stream_idx_ < 0) {
            set_error("No video stream found", video_stream_idx_);
            return false;
        }

        if (fmt_ctx_->duration != AV_NOPTS_VALUE) {
            duration_ms_ = fmt_ctx_->duration / AV_TIME_BASE * 1000;
        }

        if (fmt_ctx_->iformat->flags & AVFMT_NOBINSEARCH) {
            seekable_ = false;
        }

        return true;
    }

    bool init_decoder(int hw_type) {
        AVStream* video_stream = fmt_ctx_->streams[video_stream_idx_];
        const AVCodec* codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
        if (!codec) {
            set_error("Failed to find decoder", 0);
            return false;
        }

        codec_ctx_ = avcodec_alloc_context3(codec);
        if (!codec_ctx_) {
            set_error("Failed to allocate codec context", 0);
            return false;
        }

        int ret = avcodec_parameters_to_context(codec_ctx_, video_stream->codecpar);
        if (ret < 0) {
            set_error("Failed to copy codec parameters", ret);
            return false;
        }

        hw_type_ = hw_type;
        if (!try_hardware_accel()) {
            VV_INFO("Hardware acceleration failed, using software decode");
        }

        ret = avcodec_open2(codec_ctx_, codec, nullptr);
        if (ret < 0) {
            set_error("Failed to open codec", ret);
            return false;
        }

        width_ = codec_ctx_->width;
        height_ = codec_ctx_->height;

        return true;
    }

    bool try_hardware_accel() {
        static const AVHWDeviceType device_types[] = {
            AV_HWDEVICE_TYPE_D3D11VA,
            AV_HWDEVICE_TYPE_CUDA,
            AV_HWDEVICE_TYPE_NONE
        };

        for (int i = 0; device_types[i] != AV_HWDEVICE_TYPE_NONE; ++i) {
            if (hw_type_ == 2 && device_types[i] != AV_HWDEVICE_TYPE_CUDA) continue;
            if (hw_type_ == 1 && device_types[i] != AV_HWDEVICE_TYPE_D3D11VA) continue;

            int ret = av_hwdevice_ctx_create(&hw_device_ctx_, device_types[i], nullptr, nullptr, 0);
            if (ret != 0) {
                char err[128];
                av_strerror(ret, err, sizeof(err));
                VV_DEBUG("Failed to create {} device: {}",
                        av_hwdevice_get_type_name(device_types[i]), err);
                continue;
            }

            codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);

            for (int j = 0;; ++j) {
                const AVCodecHWConfig* config = avcodec_get_hw_config(codec_ctx_->codec, j);
                if (!config) break;
                if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                    config->device_type == device_types[i]) {
                    hw_pixel_format_ = config->pix_fmt;
                    VV_INFO("Using hardware pixel format: {} ({})",
                           static_cast<int>(hw_pixel_format_), av_hwdevice_get_type_name(device_types[i]));
                    return true;
                }
            }

            av_buffer_unref(&hw_device_ctx_);
            hw_device_ctx_ = nullptr;
        }

        return false;
    }

    bool init_texture_interop() {
        VV_DEBUG("init_texture_interop: entered");

        if (!codec_ctx_) {
            VV_DEBUG("init_texture_interop: no codec_ctx_");
            return false;
        }

        // Check if hardware decoding is available
        if (!hw_device_ctx_ || hw_pixel_format_ == AV_PIX_FMT_NONE) {
            VV_DEBUG("init_texture_interop: using software decode path (no hw context)");
            is_software_decode_ = true;
            return init_software_texture();
        }

        texture_interop_ = std::make_unique<TextureInterop>();

        // Get D3D11 device from FFmpeg's hardware context
        ID3D11Device* d3d11_device = nullptr;
        AVHWDeviceContext* device_ctx = (AVHWDeviceContext*)hw_device_ctx_->data;
        if (device_ctx->type == AV_HWDEVICE_TYPE_D3D11VA) {
            AVD3D11VADeviceContext* d3d11va_ctx = (AVD3D11VADeviceContext*)device_ctx->hwctx;
            d3d11_device = d3d11va_ctx->device;
            VV_DEBUG("Got D3D11 device from FFmpeg: {:p}", (void*)d3d11_device);
        }

        if (!texture_interop_->initialize(d3d11_device)) {
            VV_DEBUG("init_texture_interop: TextureInterop::initialize() failed");
            texture_interop_.reset();
            return false;
        }

        VV_DEBUG("init_texture_interop: success");
        return true;
    }

    bool init_software_texture() {
        // Create OpenGL texture for software decoded frames
        glGenTextures(1, &sw_texture_id_);
        if (!sw_texture_id_) {
            VV_ERROR("Failed to create software decode texture");
            return false;
        }

        glBindTexture(GL_TEXTURE_2D, sw_texture_id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        VV_DEBUG("Created software decode texture {} ({}x{})", sw_texture_id_, width_, height_);
        return true;
    }

    bool upload_software_frame(AVFrame* frame) {
        if (!sw_texture_id_ || !frame || !frame->data[0]) {
            return false;
        }

        // Allocate conversion buffer if needed
        if (!sw_rgba_buffer_ || sw_buffer_width_ != frame->width || sw_buffer_height_ != frame->height) {
            sw_rgba_buffer_.reset(new uint8_t[frame->width * frame->height * 4]);
            sw_buffer_width_ = frame->width;
            sw_buffer_height_ = frame->height;
        }

        // Convert frame to RGBA
        const int width = frame->width;
        const int height = frame->height;

        AVPixelFormat pix_fmt = static_cast<AVPixelFormat>(frame->format);

        if (pix_fmt == AV_PIX_FMT_YUV420P || pix_fmt == AV_PIX_FMT_YUVJ420P) {
            const uint8_t* y_data = frame->data[0];
            const uint8_t* u_data = frame->data[1];
            const uint8_t* v_data = frame->data[2];
            int y_linesize = frame->linesize[0];
            int u_linesize = frame->linesize[1];
            int v_linesize = frame->linesize[2];

            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int y_idx = y * y_linesize + x;
                    int uv_x = x / 2;
                    int uv_y = y / 2;
                    int u_idx = uv_y * u_linesize + uv_x;
                    int v_idx = uv_y * v_linesize + uv_x;

                    uint8_t y_val = y_data[y_idx];
                    uint8_t u_val = u_data[u_idx];
                    uint8_t v_val = v_data[v_idx];

                    // BT.601 conversion (for JPEG use BT.601 full range)
                    int c = y_val - 16;
                    int d = u_val - 128;
                    int e = v_val - 128;

                    int r = (298 * c + 409 * e + 128) >> 8;
                    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                    int b = (298 * c + 516 * d + 128) >> 8;

                    r = r < 0 ? 0 : (r > 255 ? 255 : r);
                    g = g < 0 ? 0 : (g > 255 ? 255 : g);
                    b = b < 0 ? 0 : (b > 255 ? 255 : b);

                    int dst_idx = (y * width + x) * 4;
                    sw_rgba_buffer_[dst_idx + 0] = r;
                    sw_rgba_buffer_[dst_idx + 1] = g;
                    sw_rgba_buffer_[dst_idx + 2] = b;
                    sw_rgba_buffer_[dst_idx + 3] = 255;
                }
            }
        } else if (pix_fmt == AV_PIX_FMT_NV12 || pix_fmt == AV_PIX_FMT_NV21) {
            const uint8_t* y_data = frame->data[0];
            const uint8_t* uv_data = frame->data[1];
            int y_linesize = frame->linesize[0];
            int uv_linesize = frame->linesize[1];

            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int y_idx = y * y_linesize + x;
                    int uv_x = x / 2;
                    int uv_y = y / 2;
                    int uv_idx = uv_y * uv_linesize + uv_x * 2;

                    uint8_t y_val = y_data[y_idx];
                    uint8_t u_val = uv_data[uv_idx + 0];
                    uint8_t v_val = uv_data[uv_idx + 1];

                    int c = y_val - 16;
                    int d = u_val - 128;
                    int e = v_val - 128;

                    int r = (298 * c + 409 * e + 128) >> 8;
                    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
                    int b = (298 * c + 516 * d + 128) >> 8;

                    r = r < 0 ? 0 : (r > 255 ? 255 : r);
                    g = g < 0 ? 0 : (g > 255 ? 255 : g);
                    b = b < 0 ? 0 : (b > 255 ? 255 : b);

                    int dst_idx = (y * width + x) * 4;
                    sw_rgba_buffer_[dst_idx + 0] = r;
                    sw_rgba_buffer_[dst_idx + 1] = g;
                    sw_rgba_buffer_[dst_idx + 2] = b;
                    sw_rgba_buffer_[dst_idx + 3] = 255;
                }
            }
        } else if (pix_fmt == AV_PIX_FMT_BGR0 || pix_fmt == AV_PIX_FMT_BGRA) {
            // Direct copy for BGRA/BGR0
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int src_idx = y * frame->linesize[0] + x * 4;
                    int dst_idx = (y * width + x) * 4;
                    sw_rgba_buffer_[dst_idx + 0] = frame->data[0][src_idx + 2]; // R
                    sw_rgba_buffer_[dst_idx + 1] = frame->data[0][src_idx + 1]; // G
                    sw_rgba_buffer_[dst_idx + 2] = frame->data[0][src_idx + 0]; // B
                    sw_rgba_buffer_[dst_idx + 3] = 255;
                }
            }
        } else if (pix_fmt == AV_PIX_FMT_RGB24) {
            for (int y = 0; y < height; y++) {
                for (int x = 0; x < width; x++) {
                    int src_idx = y * frame->linesize[0] + x * 3;
                    int dst_idx = (y * width + x) * 4;
                    sw_rgba_buffer_[dst_idx + 0] = frame->data[0][src_idx + 0];
                    sw_rgba_buffer_[dst_idx + 1] = frame->data[0][src_idx + 1];
                    sw_rgba_buffer_[dst_idx + 2] = frame->data[0][src_idx + 2];
                    sw_rgba_buffer_[dst_idx + 3] = 255;
                }
            }
        } else {
            VV_WARN("Unsupported pixel format for software decode: {}", static_cast<int>(pix_fmt));
            return false;
        }

        // Upload to OpenGL texture
        glBindTexture(GL_TEXTURE_2D, sw_texture_id_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, sw_rgba_buffer_.get());
        glBindTexture(GL_TEXTURE_2D, 0);

        return true;
    }

    void cleanup() {
        texture_interop_.reset();

        // Cleanup software decode resources
        if (sw_texture_id_) {
            glDeleteTextures(1, &sw_texture_id_);
            sw_texture_id_ = 0;
        }
        sw_rgba_buffer_.reset();
        sw_buffer_width_ = 0;
        sw_buffer_height_ = 0;
        is_software_decode_ = false;

        if (codec_ctx_) {
            avcodec_free_context(&codec_ctx_);
            codec_ctx_ = nullptr;
        }
        if (hw_device_ctx_) {
            av_buffer_unref(&hw_device_ctx_);
            hw_device_ctx_ = nullptr;
        }
        if (fmt_ctx_) {
            avformat_close_input(&fmt_ctx_);
            fmt_ctx_ = nullptr;
        }

        video_stream_idx_ = -1;
        width_ = 0;
        height_ = 0;
    }

    bool decode_frame() {
        if (!fmt_ctx_ || !codec_ctx_) {
            return false;
        }

        while (true) {
            int ret = av_read_frame(fmt_ctx_, pkt_);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    eof_ = true;
                    return false;
                }
                set_error("Failed to read frame", ret);
                return false;
            }

            if (pkt_->stream_index != video_stream_idx_) {
                av_packet_unref(pkt_);
                continue;
            }

            ret = avcodec_send_packet(codec_ctx_, pkt_);
            av_packet_unref(pkt_);

            if (ret < 0) {
                set_error("Failed to send packet", ret);
                return false;
            }

            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == 0) {
                current_pts_ms_ = calculate_pts();

                VV_TRACE("Frame decoded: format={}, hw_format={}, is_sw={}, has_interop={}",
                       frame_->format, static_cast<int>(hw_pixel_format_), is_software_decode_, texture_interop_ ? 1 : 0);

                if (is_software_decode_) {
                    // Software decode path: upload frame data to OpenGL texture
                    if (!upload_software_frame(frame_)) {
                        set_error("Failed to upload software frame", 0);
                        return false;
                    }
                    texture_id_ = sw_texture_id_;
                    VV_TRACE("Software texture ID: {}", texture_id_);
                } else if (frame_->format == hw_pixel_format_ && texture_interop_) {
                    if (!texture_interop_->bind_frame(frame_)) {
                        set_error("Failed to bind hardware frame", 0);
                        return false;
                    }
                    texture_id_ = texture_interop_->get_texture_id();
                    VV_TRACE("Texture ID: {}", texture_id_);
                }

                return true;
            } else if (ret == AVERROR(EAGAIN)) {
                continue;
            } else if (ret == AVERROR_EOF) {
                eof_ = true;
                return false;
            } else {
                set_error("Failed to receive frame", ret);
                return false;
            }
        }
    }

    // ==================== 异步/可取消 API ====================

    bool decode_frame_async(CancelToken& cancel_token) {
        if (!fmt_ctx_ || !codec_ctx_) {
            return false;
        }

        while (true) {
            // 检查取消
            if (cancel_token.is_cancelled()) {
                VV_DEBUG("decode_frame_async: cancelled");
                return false;
            }

            int ret = av_read_frame(fmt_ctx_, pkt_);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    eof_ = true;
                    return false;
                }
                set_error("Failed to read frame", ret);
                return false;
            }

            if (pkt_->stream_index != video_stream_idx_) {
                av_packet_unref(pkt_);
                continue;
            }

            ret = avcodec_send_packet(codec_ctx_, pkt_);
            av_packet_unref(pkt_);

            if (ret < 0) {
                set_error("Failed to send packet", ret);
                return false;
            }

            ret = avcodec_receive_frame(codec_ctx_, frame_);
            if (ret == 0) {
                current_pts_ms_ = calculate_pts();
                has_pending_frame_ = true;
                VV_TRACE("Frame decoded async: pts={}ms, pending={}", current_pts_ms_, has_pending_frame_);
                return true;
            } else if (ret == AVERROR(EAGAIN)) {
                continue;
            } else if (ret == AVERROR_EOF) {
                eof_ = true;
                return false;
            } else {
                set_error("Failed to receive frame", ret);
                return false;
            }
        }
    }

    bool seek_frame_precise_async(int64_t timestamp_ms, CancelToken& cancel_token) {
        if (!fmt_ctx_ || !seekable_) return false;

        AVRational time_base = fmt_ctx_->streams[video_stream_idx_]->time_base;
        int64_t ts = timestamp_ms * 1000;  // ms -> us
        int64_t target_ts = av_rescale_q(ts, AV_TIME_BASE_Q, time_base);

        // 检查取消
        if (cancel_token.is_cancelled()) {
            VV_DEBUG("seek_frame_precise_async: cancelled before seek");
            return false;
        }

        // Step 1: Seek to nearest keyframe before target
        int ret = av_seek_frame(fmt_ctx_, video_stream_idx_, target_ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) return false;

        avcodec_flush_buffers(codec_ctx_);
        eof_ = false;

        // Step 2: Decode frames until we reach the target timestamp or later
        int64_t target_ms = timestamp_ms;
        int64_t last_valid_pts_ms = -1;

        while (true) {
            // 每次循环检查取消
            if (cancel_token.is_cancelled()) {
                VV_DEBUG("seek_frame_precise_async: cancelled during decode at {}ms", last_valid_pts_ms);
                return false;
            }

            ret = av_read_frame(fmt_ctx_, pkt_);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    eof_ = true;
                    break;
                }
                return false;
            }

            if (pkt_->stream_index != video_stream_idx_) {
                av_packet_unref(pkt_);
                continue;
            }

            ret = avcodec_send_packet(codec_ctx_, pkt_);
            av_packet_unref(pkt_);

            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                return false;
            }

            while (true) {
                // 内层循环也检查取消
                if (cancel_token.is_cancelled()) {
                    VV_DEBUG("seek_frame_precise_async: cancelled in receive loop");
                    return false;
                }

                ret = avcodec_receive_frame(codec_ctx_, frame_);
                if (ret == AVERROR(EAGAIN)) {
                    break;
                } else if (ret == AVERROR_EOF) {
                    eof_ = true;
                    break;
                } else if (ret < 0) {
                    return false;
                }

                int64_t frame_pts_ms = calculate_pts();

                // If we've passed the target, we need to stop
                if (frame_pts_ms > target_ms) {
                    if (last_valid_pts_ms >= 0) {
                        // 需要重新 seek 获取之前的帧
                        // 注意：这里也检查取消
                        if (cancel_token.is_cancelled()) {
                            return false;
                        }
                        return seek_to_frame_before_async(target_ms, last_valid_pts_ms, cancel_token);
                    }
                    current_pts_ms_ = frame_pts_ms;
                    has_pending_frame_ = true;
                    return true;
                }

                // This frame is at or before target
                last_valid_pts_ms = frame_pts_ms;
                current_pts_ms_ = frame_pts_ms;
                has_pending_frame_ = true;

                // If exactly at target or very close, we're done
                if (frame_pts_ms == target_ms || target_ms - frame_pts_ms < 10) {
                    return true;
                }
            }

            if (eof_) break;
        }

        return last_valid_pts_ms >= 0;
    }

    bool seek_to_frame_before_async(int64_t target_ms, int64_t known_frame_ms, CancelToken& cancel_token) {
        AVRational time_base = fmt_ctx_->streams[video_stream_idx_]->time_base;
        int64_t seek_ts = (known_frame_ms - 50) * 1000;  // 50ms before
        int64_t target_ts = av_rescale_q(seek_ts, AV_TIME_BASE_Q, time_base);

        if (cancel_token.is_cancelled()) {
            return false;
        }

        int ret = av_seek_frame(fmt_ctx_, video_stream_idx_, target_ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) return false;

        avcodec_flush_buffers(codec_ctx_);
        eof_ = false;

        while (true) {
            if (cancel_token.is_cancelled()) {
                return false;
            }

            ret = av_read_frame(fmt_ctx_, pkt_);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    eof_ = true;
                    break;
                }
                return false;
            }

            if (pkt_->stream_index != video_stream_idx_) {
                av_packet_unref(pkt_);
                continue;
            }

            ret = avcodec_send_packet(codec_ctx_, pkt_);
            av_packet_unref(pkt_);

            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                return false;
            }

            while (true) {
                if (cancel_token.is_cancelled()) {
                    return false;
                }

                ret = avcodec_receive_frame(codec_ctx_, frame_);
                if (ret == AVERROR(EAGAIN)) {
                    break;
                } else if (ret == AVERROR_EOF || ret < 0) {
                    break;
                }

                int64_t frame_pts_ms = calculate_pts();

                if (frame_pts_ms >= target_ms) {
                    return true;
                }

                current_pts_ms_ = frame_pts_ms;
                has_pending_frame_ = true;
            }
        }

        return true;
    }

    bool has_pending_frame() const {
        return has_pending_frame_;
    }

    bool upload_pending_frame() {
        if (!has_pending_frame_ || !frame_) {
            return false;
        }

        VV_TRACE("upload_pending_frame: format={}, is_sw={}, has_interop={}",
               frame_->format, is_software_decode_, texture_interop_ ? 1 : 0);

        if (is_software_decode_) {
            if (!upload_software_frame(frame_)) {
                set_error("Failed to upload software frame", 0);
                return false;
            }
            texture_id_ = sw_texture_id_;
        } else if (frame_->format == hw_pixel_format_ && texture_interop_) {
            if (!texture_interop_->bind_frame(frame_)) {
                set_error("Failed to bind hardware frame", 0);
                return false;
            }
            texture_id_ = texture_interop_->get_texture_id();
        }

        has_pending_frame_ = false;
        return true;
    }

    bool seek_frame(int64_t timestamp_ms) {
        if (!fmt_ctx_ || !seekable_) return false;

        AVRational time_base = fmt_ctx_->streams[video_stream_idx_]->time_base;
        int64_t ts = timestamp_ms * 1000;
        int64_t target_ts = av_rescale_q(ts, AV_TIME_BASE_Q, time_base);

        int ret = av_seek_frame(fmt_ctx_, video_stream_idx_, target_ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) return false;

        avcodec_flush_buffers(codec_ctx_);
        eof_ = false;
        return true;
    }

    bool seek_frame_precise(int64_t timestamp_ms) {
        if (!fmt_ctx_ || !seekable_) return false;

        AVRational time_base = fmt_ctx_->streams[video_stream_idx_]->time_base;
        int64_t ts = timestamp_ms * 1000;  // ms -> us
        int64_t target_ts = av_rescale_q(ts, AV_TIME_BASE_Q, time_base);

        // Step 1: Seek to nearest keyframe before target
        int ret = av_seek_frame(fmt_ctx_, video_stream_idx_, target_ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) return false;

        avcodec_flush_buffers(codec_ctx_);
        eof_ = false;

        // Step 2: Decode frames until we reach the target timestamp or later
        // We want the last frame with pts <= target
        int64_t target_ms = timestamp_ms;
        int64_t last_valid_pts_ms = -1;

        while (true) {
            ret = av_read_frame(fmt_ctx_, pkt_);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    eof_ = true;
                    break;
                }
                return false;
            }

            if (pkt_->stream_index != video_stream_idx_) {
                av_packet_unref(pkt_);
                continue;
            }

            ret = avcodec_send_packet(codec_ctx_, pkt_);
            av_packet_unref(pkt_);

            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                return false;
            }

            while (true) {
                ret = avcodec_receive_frame(codec_ctx_, frame_);
                if (ret == AVERROR(EAGAIN)) {
                    break;
                } else if (ret == AVERROR_EOF) {
                    eof_ = true;
                    break;
                } else if (ret < 0) {
                    return false;
                }

                int64_t frame_pts_ms = calculate_pts();

                // If we've passed the target, we need to stop
                if (frame_pts_ms > target_ms) {
                    // We've decoded past the target
                    // The previous frame (if any) is the one we want
                    if (last_valid_pts_ms >= 0) {
                        // Seek back to get the previous frame again
                        // This is the frame just before target
                        return seek_to_frame_before(target_ms, last_valid_pts_ms);
                    }
                    // No valid frame found before target, use this one anyway
                    current_pts_ms_ = frame_pts_ms;
                    upload_decoded_frame();
                    return true;
                }

                // This frame is at or before target
                last_valid_pts_ms = frame_pts_ms;
                current_pts_ms_ = frame_pts_ms;
                upload_decoded_frame();

                // If exactly at target or very close, we're done
                if (frame_pts_ms == target_ms || target_ms - frame_pts_ms < 10) {
                    return true;
                }
            }

            if (eof_) break;
        }

        // If we have a valid frame, return success
        return last_valid_pts_ms >= 0;
    }

    bool seek_to_frame_before(int64_t target_ms, int64_t known_frame_ms) {
        // Seek to a bit before the known frame time
        AVRational time_base = fmt_ctx_->streams[video_stream_idx_]->time_base;
        int64_t seek_ts = (known_frame_ms - 50) * 1000;  // 50ms before
        int64_t target_ts = av_rescale_q(seek_ts, AV_TIME_BASE_Q, time_base);

        int ret = av_seek_frame(fmt_ctx_, video_stream_idx_, target_ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) return false;

        avcodec_flush_buffers(codec_ctx_);
        eof_ = false;

        // Decode until we get to the frame just before target
        while (true) {
            ret = av_read_frame(fmt_ctx_, pkt_);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    eof_ = true;
                    break;
                }
                return false;
            }

            if (pkt_->stream_index != video_stream_idx_) {
                av_packet_unref(pkt_);
                continue;
            }

            ret = avcodec_send_packet(codec_ctx_, pkt_);
            av_packet_unref(pkt_);

            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                return false;
            }

            while (true) {
                ret = avcodec_receive_frame(codec_ctx_, frame_);
                if (ret == AVERROR(EAGAIN)) {
                    break;
                } else if (ret == AVERROR_EOF || ret < 0) {
                    break;
                }

                int64_t frame_pts_ms = calculate_pts();

                // Stop when we reach a frame at or past target
                if (frame_pts_ms >= target_ms) {
                    // Use the previous frame we saved
                    return true;
                }

                current_pts_ms_ = frame_pts_ms;
                upload_decoded_frame();
            }
        }

        return true;
    }

    void upload_decoded_frame() {
        if (is_software_decode_) {
            upload_software_frame(frame_);
        } else if (frame_->format == hw_pixel_format_ && texture_interop_) {
            texture_interop_->bind_frame(frame_);
            texture_id_ = texture_interop_->get_texture_id();
        }
    }

    int64_t calculate_pts() {
        AVStream* video_stream = fmt_ctx_->streams[video_stream_idx_];
        AVRational time_base = video_stream->time_base;

        if (frame_->pts != AV_NOPTS_VALUE) {
            return av_rescale_q(frame_->pts, time_base, AV_TIME_BASE_Q) / 1000;
        } else if (frame_->best_effort_timestamp != AV_NOPTS_VALUE) {
            return frame_->best_effort_timestamp / 1000;
        }
        return current_pts_ms_;
    }

    void set_error(const char* msg, int av_err) {
        error_ = true;
        char errbuf[256] = {0};
        if (av_err < 0) {
            av_strerror(av_err, errbuf, sizeof(errbuf));
            error_msg_ = std::string(msg) + ": " + errbuf;
        } else {
            error_msg_ = msg;
        }
    }

    // FFmpeg resources
    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;

    // Stream info
    int video_stream_idx_ = -1;
    AVPixelFormat hw_pixel_format_ = AV_PIX_FMT_NONE;
    int64_t duration_ms_ = 0;
    bool seekable_ = true;

    // Frame state
    int64_t current_pts_ms_ = 0;
    bool eof_ = false;
    bool error_ = false;
    std::string error_msg_;

    // Video dimensions
    int width_ = 0;
    int height_ = 0;

    // Texture interop
    std::unique_ptr<TextureInterop> texture_interop_;
    uint32_t texture_id_ = 0;
    void* gl_context_ = nullptr;

    // Software decode path
    bool is_software_decode_ = false;
    uint32_t sw_texture_id_ = 0;
    std::unique_ptr<uint8_t[]> sw_rgba_buffer_;
    int sw_buffer_width_ = 0;
    int sw_buffer_height_ = 0;

    // Async decode state
    bool has_pending_frame_ = false;  // frame_ 中有待上传的帧

    int hw_type_ = 0;
};

// ==================== HardwareDecoder ====================

HardwareDecoder::HardwareDecoder(const std::string& source_url)
    : impl_(std::make_unique<Impl>()) {
    if (!impl_->open_source(source_url)) {
    }
}

HardwareDecoder::~HardwareDecoder() = default;

HardwareDecoder::HardwareDecoder(HardwareDecoder&&) noexcept = default;
HardwareDecoder& HardwareDecoder::operator=(HardwareDecoder&&) noexcept = default;

bool HardwareDecoder::initialize(int hw_device_type) {
    return impl_->init_decoder(hw_device_type);
}

void HardwareDecoder::set_opengl_context(void* gl_context) {
    VV_DEBUG("set_opengl_context called");
    impl_->gl_context_ = gl_context;
    bool result = impl_->init_texture_interop();
    VV_DEBUG("init_texture_interop returned: {}, has_interop={}", result, impl_->texture_interop_ ? 1 : 0);
}

bool HardwareDecoder::seek_to(int64_t timestamp_ms) {
    return impl_->seek_frame(timestamp_ms);
}

bool HardwareDecoder::seek_to_precise(int64_t timestamp_ms) {
    return impl_->seek_frame_precise(timestamp_ms);
}

bool HardwareDecoder::has_pending_frame() const {
    return impl_->has_pending_frame();
}

bool HardwareDecoder::upload_pending_frame() {
    return impl_->upload_pending_frame();
}

// ==================== Internal API (for DecodeWorker) ====================

bool HardwareDecoder::decode_frame_internal() {
    // 使用 async 版本的逻辑，但不需要 CancelToken
    if (!impl_->fmt_ctx_ || !impl_->codec_ctx_) {
        return false;
    }

    while (true) {
        int ret = av_read_frame(impl_->fmt_ctx_, impl_->pkt_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                impl_->eof_ = true;
                return false;
            }
            impl_->set_error("Failed to read frame", ret);
            return false;
        }

        if (impl_->pkt_->stream_index != impl_->video_stream_idx_) {
            av_packet_unref(impl_->pkt_);
            continue;
        }

        ret = avcodec_send_packet(impl_->codec_ctx_, impl_->pkt_);
        av_packet_unref(impl_->pkt_);

        if (ret < 0) {
            impl_->set_error("Failed to send packet", ret);
            return false;
        }

        ret = avcodec_receive_frame(impl_->codec_ctx_, impl_->frame_);
        if (ret == 0) {
            impl_->current_pts_ms_ = impl_->calculate_pts();
            impl_->has_pending_frame_ = true;
            VV_TRACE("decode_frame_internal: pts={}ms, pending={}",
                    impl_->current_pts_ms_, impl_->has_pending_frame_);
            return true;
        } else if (ret == AVERROR(EAGAIN)) {
            continue;
        } else if (ret == AVERROR_EOF) {
            impl_->eof_ = true;
            return false;
        } else {
            impl_->set_error("Failed to receive frame", ret);
            return false;
        }
    }
}

bool HardwareDecoder::seek_to_precise_internal(int64_t timestamp_ms) {
    if (!impl_->fmt_ctx_ || !impl_->seekable_) return false;

    AVRational time_base = impl_->fmt_ctx_->streams[impl_->video_stream_idx_]->time_base;
    int64_t ts = timestamp_ms * 1000;  // ms -> us
    int64_t target_ts = av_rescale_q(ts, AV_TIME_BASE_Q, time_base);

    // Step 1: Seek to nearest keyframe before target
    int ret = av_seek_frame(impl_->fmt_ctx_, impl_->video_stream_idx_, target_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) return false;

    avcodec_flush_buffers(impl_->codec_ctx_);
    impl_->eof_ = false;

    // Step 2: Decode frames until we reach the target timestamp or later
    int64_t target_ms = timestamp_ms;
    int64_t last_valid_pts_ms = -1;

    while (true) {
        ret = av_read_frame(impl_->fmt_ctx_, impl_->pkt_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                impl_->eof_ = true;
                break;
            }
            return false;
        }

        if (impl_->pkt_->stream_index != impl_->video_stream_idx_) {
            av_packet_unref(impl_->pkt_);
            continue;
        }

        ret = avcodec_send_packet(impl_->codec_ctx_, impl_->pkt_);
        av_packet_unref(impl_->pkt_);

        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            return false;
        }

        while (true) {
            ret = avcodec_receive_frame(impl_->codec_ctx_, impl_->frame_);
            if (ret == AVERROR(EAGAIN)) {
                break;
            } else if (ret == AVERROR_EOF) {
                impl_->eof_ = true;
                break;
            } else if (ret < 0) {
                return false;
            }

            int64_t frame_pts_ms = impl_->calculate_pts();

            // If we've passed the target, we need to stop
            if (frame_pts_ms > target_ms) {
                if (last_valid_pts_ms >= 0) {
                    // Seek back to get the previous frame again
                    return seek_to_frame_before_internal(target_ms, last_valid_pts_ms);
                }
                impl_->current_pts_ms_ = frame_pts_ms;
                impl_->has_pending_frame_ = true;
                return true;
            }

            // This frame is at or before target
            last_valid_pts_ms = frame_pts_ms;
            impl_->current_pts_ms_ = frame_pts_ms;
            impl_->has_pending_frame_ = true;

            // If exactly at target or very close, we're done
            if (frame_pts_ms == target_ms || target_ms - frame_pts_ms < 10) {
                return true;
            }
        }

        if (impl_->eof_) break;
    }

    return last_valid_pts_ms >= 0;
}

bool HardwareDecoder::seek_to_keyframe_internal(int64_t timestamp_ms) {
    if (!impl_->fmt_ctx_ || !impl_->seekable_) return false;

    AVRational time_base = impl_->fmt_ctx_->streams[impl_->video_stream_idx_]->time_base;
    int64_t ts = timestamp_ms * 1000;
    int64_t target_ts = av_rescale_q(ts, AV_TIME_BASE_Q, time_base);

    int ret = av_seek_frame(impl_->fmt_ctx_, impl_->video_stream_idx_, target_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) return false;

    avcodec_flush_buffers(impl_->codec_ctx_);
    impl_->eof_ = false;
    return true;
}

bool HardwareDecoder::seek_to_frame_before_internal(int64_t target_ms, int64_t known_frame_ms) {
    // Seek to a bit before the known frame time
    AVRational time_base = impl_->fmt_ctx_->streams[impl_->video_stream_idx_]->time_base;
    int64_t seek_ts = (known_frame_ms - 50) * 1000;  // 50ms before
    int64_t seek_target_ts = av_rescale_q(seek_ts, AV_TIME_BASE_Q, time_base);

    int ret = av_seek_frame(impl_->fmt_ctx_, impl_->video_stream_idx_, seek_target_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) return false;

    avcodec_flush_buffers(impl_->codec_ctx_);
    impl_->eof_ = false;

    while (true) {
        ret = av_read_frame(impl_->fmt_ctx_, impl_->pkt_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                impl_->eof_ = true;
                break;
            }
            return false;
        }

        if (impl_->pkt_->stream_index != impl_->video_stream_idx_) {
            av_packet_unref(impl_->pkt_);
            continue;
        }

        ret = avcodec_send_packet(impl_->codec_ctx_, impl_->pkt_);
        av_packet_unref(impl_->pkt_);

        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            return false;
        }

        while (true) {
            ret = avcodec_receive_frame(impl_->codec_ctx_, impl_->frame_);
            if (ret == AVERROR(EAGAIN)) {
                break;
            } else if (ret == AVERROR_EOF || ret < 0) {
                break;
            }

            int64_t frame_pts_ms = impl_->calculate_pts();

            if (frame_pts_ms >= target_ms) {
                return true;
            }

            impl_->current_pts_ms_ = frame_pts_ms;
            impl_->has_pending_frame_ = true;
        }
    }

    return true;
}

uint32_t HardwareDecoder::get_texture_id() const {
    return impl_->texture_id_;
}

int64_t HardwareDecoder::get_current_pts_ms() const {
    return impl_->current_pts_ms_;
}

int64_t HardwareDecoder::get_duration_ms() const {
    return impl_->duration_ms_;
}

bool HardwareDecoder::is_seekable() const {
    return impl_->seekable_;
}

bool HardwareDecoder::is_eof() const {
    return impl_->eof_;
}

AVFrame* HardwareDecoder::take_pending_frame() {
    if (!impl_->has_pending_frame_) {
        return nullptr;
    }

    // 转移帧所有权
    AVFrame* frame = impl_->frame_;
    impl_->frame_ = av_frame_alloc();  // 分配新帧供下次使用
    impl_->has_pending_frame_ = false;

    VV_TRACE("take_pending_frame: transferred frame pts={}ms", impl_->current_pts_ms_);
    return frame;
}

void HardwareDecoder::set_pending_frame(AVFrame* frame) {
    if (!frame) return;

    // 释放旧帧（如果有）
    if (impl_->frame_) {
        av_frame_free(&impl_->frame_);
    }

    impl_->frame_ = frame;
    impl_->has_pending_frame_ = true;
    impl_->current_pts_ms_ = impl_->calculate_pts();

    VV_TRACE("set_pending_frame: set frame pts={}ms", impl_->current_pts_ms_);
}

bool HardwareDecoder::has_error() const {
    return impl_->error_;
}

std::string HardwareDecoder::get_error_message() const {
    return impl_->error_msg_;
}

int HardwareDecoder::get_width() const {
    return impl_->width_;
}

int HardwareDecoder::get_height() const {
    return impl_->height_;
}

} // namespace voidview
