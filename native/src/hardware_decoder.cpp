#include "voidview_native/hardware_decoder.hpp"
#include "voidview_native/texture_interop.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>

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
            fprintf(stderr, "Hardware acceleration failed, using software decode\n");
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
                fprintf(stderr, "Failed to create %s device: %s\n",
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
                    printf("Using hardware pixel format: %d (%s)\n",
                           hw_pixel_format_, av_hwdevice_get_type_name(device_types[i]));
                    return true;
                }
            }

            av_buffer_unref(&hw_device_ctx_);
            hw_device_ctx_ = nullptr;
        }

        return false;
    }

    bool init_texture_interop() {
        printf("init_texture_interop: entered\n");
        fflush(stdout);

        if (!codec_ctx_) {
            printf("init_texture_interop: no codec_ctx_\n");
            return false;
        }

        texture_interop_ = std::make_unique<TextureInterop>();

        // Get D3D11 device from FFmpeg's hardware context
        ID3D11Device* d3d11_device = nullptr;
        if (hw_device_ctx_ && hw_pixel_format_ != AV_PIX_FMT_NONE) {
            AVHWDeviceContext* device_ctx = (AVHWDeviceContext*)hw_device_ctx_->data;
            if (device_ctx->type == AV_HWDEVICE_TYPE_D3D11VA) {
                AVD3D11VADeviceContext* d3d11va_ctx = (AVD3D11VADeviceContext*)device_ctx->hwctx;
                d3d11_device = d3d11va_ctx->device;
                printf("Got D3D11 device from FFmpeg: %p\n", d3d11_device);
            }
        }

        if (!texture_interop_->initialize(d3d11_device)) {
            printf("init_texture_interop: TextureInterop::initialize() failed\n");
            texture_interop_.reset();
            return false;
        }

        printf("init_texture_interop: success\n");
        return true;
    }

    void cleanup() {
        texture_interop_.reset();

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

                printf("Frame decoded: format=%d, hw_format=%d, has_interop=%d\n",
                       frame_->format, hw_pixel_format_, texture_interop_ ? 1 : 0);

                if (frame_->format == hw_pixel_format_ && texture_interop_) {
                    if (!texture_interop_->bind_frame(frame_)) {
                        set_error("Failed to bind hardware frame", 0);
                        return false;
                    }
                    texture_id_ = texture_interop_->get_texture_id();
                    printf("Texture ID: %u\n", texture_id_);
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
    printf("set_opengl_context called\n");
    fflush(stdout);
    impl_->gl_context_ = gl_context;
    bool result = impl_->init_texture_interop();
    printf("init_texture_interop returned: %d, has_interop=%d\n", result, impl_->texture_interop_ ? 1 : 0);
    fflush(stdout);
}

bool HardwareDecoder::decode_next_frame() {
    return impl_->decode_frame();
}

bool HardwareDecoder::seek_to(int64_t timestamp_ms) {
    return impl_->seek_frame(timestamp_ms);
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
