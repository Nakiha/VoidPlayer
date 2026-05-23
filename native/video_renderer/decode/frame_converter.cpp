#include "video_renderer/decode/frame_converter.h"
#include "video_renderer/decode/frame_color_metadata.h"
#include "video_renderer/decode/software_frame_packer.h"
#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace vr {

FrameConverter::FrameConverter()
{}

FrameConverter::~FrameConverter() = default;

D3D11SnapshotPoolStats FrameConverter::snapshot_pool_stats() const {
    return d3d11_snapshot_pool_stats(d3d11_snapshot_pool_);
}

bool FrameConverter::init_software(int src_width, int src_height, AVPixelFormat src_format) {
    width_ = src_width;
    height_ = src_height;
    src_format_ = src_format;
    is_hw_ = false;
    download_hw_to_cpu_ = false;
    hw_type_ = HwDecodeType::None;
    d3d_device_ = nullptr;
    d3d_context_ = nullptr;
    device_mutex_ = nullptr;
    d3d11_snapshot_pool_.reset();

    if (src_width <= 0 || src_height <= 0 || src_format == AV_PIX_FMT_NONE) {
        spdlog::info("[FrameConverter] Software converter will initialize from first frame "
                     "(initial params {}x{}, format={})",
                     src_width, src_height, static_cast<int>(src_format));
        return true;
    }

    if (!validate_software_frame_layout(src_width, src_height, src_format)) {
        spdlog::error("[FrameConverter] Refusing unsupported software frame geometry "
                      "({}x{}, format={})",
                      src_width, src_height, static_cast<int>(src_format));
        return false;
    }
    if (!supported_software_format(src_format)) {
        const char* name = av_get_pix_fmt_name(src_format);
        spdlog::error("[FrameConverter] Unsupported initial software pixel format {} ({}) "
                      "without libswscale",
                      static_cast<int>(src_format), name ? name : "unknown");
        return false;
    }

    spdlog::info("[FrameConverter] Software converter initialized for deterministic upload "
                 "({}x{}, format={})",
                 src_width, src_height, static_cast<int>(src_format));
    return true;
}

bool FrameConverter::init_hardware(void* d3d_device, void* d3d_context,
                                   int src_width, int src_height,
                                   HwDecodeType hw_type,
                                   bool download_to_cpu,
                                   std::recursive_mutex* device_mutex) {
    d3d_device_ = d3d_device;
    d3d_context_ = d3d_context;
    device_mutex_ = device_mutex;
    width_ = src_width;
    height_ = src_height;
    is_hw_ = true;
    download_hw_to_cpu_ = download_to_cpu;
    hw_type_ = hw_type;
    downloaded_format_ = AV_PIX_FMT_NONE;
#ifdef _WIN32
    d3d11_snapshot_pool_ =
        (!download_to_cpu && hw_type == HwDecodeType::D3D11VA)
        ? create_d3d11_snapshot_pool()
        : nullptr;
#else
    d3d11_snapshot_pool_.reset();
    if (!download_to_cpu) {
        spdlog::error("[FrameConverter] Renderer-owned hardware frames are Windows-only");
        return false;
    }
#endif

    spdlog::info("[FrameConverter] Hardware converter initialized ({}x{}, hw_type={}, download_to_cpu={})",
                 src_width, src_height,
                 hw_decode_type_name(hw_type),
                 download_hw_to_cpu_);
    return true;
}

std::optional<TextureFrame> FrameConverter::convert(AVFrame* frame) {
    TextureFrame result;
    if (!frame) {
        spdlog::error("[FrameConverter] convert called with null AVFrame");
        return std::nullopt;
    }

    result.pts_us = frame->pts;
    result.dts_us = frame->pkt_dts != AV_NOPTS_VALUE
        ? frame->pkt_dts
        : kNoTimestampUs;
    result.duration_us = frame->duration;
    result.width = frame->width;
    result.height = frame->height;
    result.is_ref = false;
    result.texture_handle = nullptr;
    result.color = color_info_from_av_frame(frame);

    if (is_hw_ && download_hw_to_cpu_) {
        AVFrame* sw_frame = av_frame_alloc();
        if (!sw_frame) {
            spdlog::error("[FrameConverter] Failed to allocate hw download frame");
            return std::nullopt;
        }

        int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
        if (ret < 0) {
            spdlog::error("[FrameConverter] av_hwframe_transfer_data failed: {:#x}",
                          static_cast<unsigned>(ret));
            av_frame_free(&sw_frame);
            return std::nullopt;
        }

        const auto sw_format = static_cast<AVPixelFormat>(sw_frame->format);
        result.color = color_info_from_av_frame(sw_frame);
        downloaded_format_ = sw_format;

        if (!convert_frame_to_cpu_nv12(sw_frame, "hw-download", result)) {
            av_frame_free(&sw_frame);
            return std::nullopt;
        }
        av_frame_free(&sw_frame);
    } else if (is_hw_) {
        if (hw_type_ == HwDecodeType::D3D11VA) {
            if (!populate_d3d11_hardware_texture_frame(frame, result)) {
                return std::nullopt;
            }
        } else {
            spdlog::error("[FrameConverter] Renderer-owned hardware frames require a platform presenter");
            return std::nullopt;
        }
    } else {
        // Software 8-bit 4:2:0 can be uploaded as its original Y/U/V planes.
        // Other supported software formats still use the deterministic packer.
        result.color = color_info_from_av_frame(frame);
        if (software_format_uses_direct_planar_yuv420(
                static_cast<AVPixelFormat>(frame->format))) {
            if (!wrap_frame_as_cpu_planar_yuv420(frame, result)) {
                return std::nullopt;
            }
        } else if (!convert_frame_to_cpu_nv12(frame, "software", result)) {
            return std::nullopt;
        }
        width_ = frame->width;
        height_ = frame->height;
        src_format_ = static_cast<AVPixelFormat>(frame->format);
    }

    return result;
}

std::optional<TextureFrame> FrameConverter::snapshot_hardware_frame(AVFrame* frame) {
    if (!is_hw_ || download_hw_to_cpu_ || hw_type_ != HwDecodeType::D3D11VA ||
        !frame || !frame->data[0]) {
        return std::nullopt;
    }

    TextureFrame result;
    result.pts_us = frame->pts;
    result.dts_us = frame->pkt_dts != AV_NOPTS_VALUE
        ? frame->pkt_dts
        : kNoTimestampUs;
    result.duration_us = frame->duration;
    result.width = frame->width;
    result.height = frame->height;
    result.color = color_info_from_av_frame(frame);
    return snapshot_d3d11_hardware_frame(
        frame,
        result,
        device_mutex_,
        d3d11_snapshot_pool_);
}

} // namespace vr
