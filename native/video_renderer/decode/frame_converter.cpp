#include "video_renderer/decode/frame_converter.h"
#include <spdlog/spdlog.h>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

namespace vr {

FrameConverter::FrameConverter()
{}

FrameConverter::~FrameConverter() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
}

bool FrameConverter::init_software(int src_width, int src_height, AVPixelFormat src_format) {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    width_ = src_width;
    height_ = src_height;
    src_format_ = src_format;
    is_hw_ = false;
    download_hw_to_cpu_ = false;
    hw_type_ = HwDecodeType::None;

    sws_ctx_ = sws_getContext(
        src_width, src_height, src_format,
        src_width, src_height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR,
        nullptr, nullptr, nullptr
    );

    if (!sws_ctx_) {
        spdlog::error("[FrameConverter] Failed to create SwsContext ({}x{}, format={})",
                      src_width, src_height, static_cast<int>(src_format));
        return false;
    }

    spdlog::info("[FrameConverter] Software converter initialized ({}x{}, format={})",
                 src_width, src_height, static_cast<int>(src_format));
    return true;
}

bool FrameConverter::init_hardware(void* d3d_device, void* d3d_context,
                                   int src_width, int src_height,
                                   HwDecodeType hw_type,
                                   bool download_to_cpu) {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    d3d_device_ = d3d_device;
    d3d_context_ = d3d_context;
    width_ = src_width;
    height_ = src_height;
    is_hw_ = true;
    download_hw_to_cpu_ = download_to_cpu;
    hw_type_ = hw_type;
    downloaded_format_ = AV_PIX_FMT_NONE;

    spdlog::info("[FrameConverter] Hardware converter initialized ({}x{}, hw_type={}, download_to_cpu={})",
                 src_width, src_height,
                 hw_type == HwDecodeType::D3D11VA ? "D3D11VA" : "unknown",
                 download_hw_to_cpu_);
    return true;
}

TextureFrame FrameConverter::convert(AVFrame* frame) {
    TextureFrame result;
    result.pts_us = frame->pts;
    result.duration_us = frame->duration;
    result.width = frame->width;
    result.height = frame->height;
    result.is_ref = false;
    result.texture_handle = nullptr;

    if (is_hw_ && download_hw_to_cpu_) {
        AVFrame* sw_frame = av_frame_alloc();
        if (!sw_frame) {
            spdlog::error("[FrameConverter] Failed to allocate hw download frame");
            return result;
        }

        int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
        if (ret < 0) {
            spdlog::error("[FrameConverter] av_hwframe_transfer_data failed: {:#x}",
                          static_cast<unsigned>(ret));
            av_frame_free(&sw_frame);
            return result;
        }

        const auto sw_format = static_cast<AVPixelFormat>(sw_frame->format);
        if (!sws_ctx_ || downloaded_format_ != sw_format) {
            if (sws_ctx_) {
                sws_freeContext(sws_ctx_);
                sws_ctx_ = nullptr;
            }
            downloaded_format_ = sw_format;
            sws_ctx_ = sws_getContext(
                sw_frame->width, sw_frame->height, sw_format,
                sw_frame->width, sw_frame->height, AV_PIX_FMT_RGBA,
                SWS_BILINEAR,
                nullptr, nullptr, nullptr);
            if (!sws_ctx_) {
                spdlog::error("[FrameConverter] Failed to create hw-download SwsContext ({}x{}, format={})",
                              sw_frame->width, sw_frame->height, static_cast<int>(sw_format));
                av_frame_free(&sw_frame);
                return result;
            }
            spdlog::info("[FrameConverter] Hardware download converter initialized ({}x{}, format={})",
                         sw_frame->width, sw_frame->height, static_cast<int>(sw_format));
        }

        result.width = sw_frame->width;
        result.height = sw_frame->height;
        size_t stride = static_cast<size_t>(sw_frame->width) * 4;
        size_t buf_size = stride * static_cast<size_t>(sw_frame->height);
        auto rgba_buf = std::make_shared<std::vector<uint8_t>>(buf_size);
        if (!rgba_buf || rgba_buf->empty()) {
            spdlog::error("[FrameConverter] Failed to allocate hw-download RGBA buffer ({} bytes)", buf_size);
            av_frame_free(&sw_frame);
            return result;
        }

        uint8_t* dst_slices[1] = { rgba_buf->data() };
        int dst_stride[1] = { static_cast<int>(stride) };
        const int converted_height = sws_scale(
            sws_ctx_,
            sw_frame->data, sw_frame->linesize,
            0, sw_frame->height,
            dst_slices, dst_stride);
        if (converted_height != sw_frame->height) {
            spdlog::warn("[FrameConverter] hw-download sws_scale converted {} rows (expected {})",
                         converted_height, sw_frame->height);
        }

        result.cpu_data = rgba_buf;
        result.texture_handle = rgba_buf->data();
        result.is_ref = false;
        result.storage = CpuRgbaFrameStorage{
            rgba_buf,
            static_cast<int>(stride),
        };
        av_frame_free(&sw_frame);
    } else if (is_hw_) {
        // frame->data[0] = ID3D11Texture2D*, frame->data[1] = array index (intptr_t)
        if (frame->data[0]) {
            result.texture_handle = frame->data[0];
            result.is_ref = true;

            if (hw_type_ == HwDecodeType::D3D11VA) {
                result.is_nv12 = true;
                result.texture_array_index = static_cast<int>(
                    reinterpret_cast<intptr_t>(frame->data[1]));
            }

            // Keep the AVFrame alive via av_frame_ref so the decoder cannot
            // reuse the hw frame pool slot while the render thread holds the
            // TextureFrame. The shared_ptr deleter calls av_frame_free when
            // the TextureFrame is discarded by the render thread.
            AVFrame* ref_frame = av_frame_alloc();
            if (ref_frame && av_frame_ref(ref_frame, frame) >= 0) {
                result.hw_frame_ref = std::shared_ptr<void>(ref_frame, [](void* p) {
                    AVFrame* f = static_cast<AVFrame*>(p);
                    av_frame_free(&f);
                });
            } else {
                spdlog::warn("[FrameConverter] Failed to ref hw frame, texture may be recycled early");
                if (ref_frame) av_frame_free(&ref_frame);
            }

            if (result.is_nv12) {
                result.storage = D3D11Nv12FrameStorage{
                    static_cast<ID3D11Texture2D*>(result.texture_handle),
                    result.texture_array_index,
                    result.hw_frame_ref,
                };
            } else {
                result.storage = D3D11TextureFrameStorage{
                    static_cast<ID3D11Texture2D*>(result.texture_handle),
                    result.hw_frame_ref,
                };
            }
        }
    } else {
        // Software path: convert to RGBA via sws_scale
        if (!sws_ctx_) {
            spdlog::error("[FrameConverter] Software converter not initialized");
            return result;
        }

        size_t stride = static_cast<size_t>(width_) * 4;
        size_t buf_size = stride * static_cast<size_t>(height_);
        auto rgba_buf = std::make_shared<std::vector<uint8_t>>(buf_size);
        if (!rgba_buf || rgba_buf->empty()) {
            spdlog::error("[FrameConverter] Failed to allocate RGBA buffer ({} bytes)", buf_size);
            return result;
        }

        uint8_t* dst_slices[1] = { rgba_buf->data() };
        int dst_stride[1] = { static_cast<int>(stride) };

        int converted_height = sws_scale(
            sws_ctx_,
            frame->data, frame->linesize,
            0, height_,
            dst_slices, dst_stride
        );

        if (converted_height != height_) {
            spdlog::warn("[FrameConverter] sws_scale converted {} rows (expected {})",
                         converted_height, height_);
        }

        result.cpu_data = rgba_buf;
        result.texture_handle = rgba_buf->data();
        result.is_ref = false;
        result.storage = CpuRgbaFrameStorage{
            rgba_buf,
            static_cast<int>(stride),
        };
    }

    return result;
}

} // namespace vr
