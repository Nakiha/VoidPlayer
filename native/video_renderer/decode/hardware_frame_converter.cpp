#include "video_renderer/decode/hardware_frame_converter.h"

#include "video_renderer/decode/frame_color_metadata.h"
#include "video_renderer/decode/software_frame_packer.h"

#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
}

namespace vr {
namespace {

TextureFrame make_texture_frame_metadata(AVFrame* frame) {
    TextureFrame result;
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
    return result;
}

} // namespace

bool HardwareFrameConverter::init(void* d3d_device, void* d3d_context,
                                  int src_width, int src_height,
                                  HwDecodeType hw_type,
                                  bool download_to_cpu,
                                  std::recursive_mutex* device_mutex) {
    (void)d3d_device;
    (void)d3d_context;
    width_ = src_width;
    height_ = src_height;
    hw_type_ = hw_type;
    download_to_cpu_ = download_to_cpu;
    downloaded_format_ = AV_PIX_FMT_NONE;
    device_mutex_ = device_mutex;

#ifdef _WIN32
    d3d11_snapshot_pool_ =
        (!download_to_cpu && hw_type == HwDecodeType::D3D11VA)
        ? create_d3d11_snapshot_pool()
        : nullptr;
#else
    d3d11_snapshot_pool_.reset();
    if (!download_to_cpu) {
        spdlog::error("[HardwareFrameConverter] Renderer-owned hardware frames are Windows-only");
        return false;
    }
#endif

    spdlog::info("[HardwareFrameConverter] Hardware converter initialized "
                 "({}x{}, hw_type={}, download_to_cpu={})",
                 width_, height_, hw_decode_type_name(hw_type_), download_to_cpu_);
    return true;
}

std::optional<TextureFrame> HardwareFrameConverter::convert(AVFrame* frame) {
    if (!frame) {
        spdlog::error("[HardwareFrameConverter] convert called with null AVFrame");
        return std::nullopt;
    }

    if (download_to_cpu_) {
        AVFrame* sw_frame = av_frame_alloc();
        if (!sw_frame) {
            spdlog::error("[HardwareFrameConverter] Failed to allocate hw download frame");
            return std::nullopt;
        }

        const int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
        if (ret < 0) {
            spdlog::error("[HardwareFrameConverter] av_hwframe_transfer_data failed: {:#x}",
                          static_cast<unsigned>(ret));
            av_frame_free(&sw_frame);
            return std::nullopt;
        }

        TextureFrame result = make_texture_frame_metadata(frame);
        result.color = color_info_from_av_frame(sw_frame);
        downloaded_format_ = static_cast<AVPixelFormat>(sw_frame->format);
        if (!convert_frame_to_cpu_nv12(sw_frame, "hw-download", result)) {
            av_frame_free(&sw_frame);
            return std::nullopt;
        }
        av_frame_free(&sw_frame);
        return result;
    }

    TextureFrame result = make_texture_frame_metadata(frame);
    if (hw_type_ == HwDecodeType::D3D11VA) {
        if (!populate_d3d11_hardware_texture_frame(frame, result)) {
            return std::nullopt;
        }
        return result;
    }

    spdlog::error("[HardwareFrameConverter] Renderer-owned hardware frames require a platform presenter");
    return std::nullopt;
}

std::optional<TextureFrame> HardwareFrameConverter::snapshot_frame(AVFrame* frame) {
    if (download_to_cpu_ || hw_type_ != HwDecodeType::D3D11VA || !frame || !frame->data[0]) {
        return std::nullopt;
    }

    TextureFrame metadata = make_texture_frame_metadata(frame);
    return snapshot_d3d11_hardware_frame(
        frame,
        metadata,
        device_mutex_,
        d3d11_snapshot_pool_);
}

D3D11SnapshotPoolStats HardwareFrameConverter::snapshot_pool_stats() const {
    return d3d11_snapshot_pool_stats(d3d11_snapshot_pool_);
}

} // namespace vr
