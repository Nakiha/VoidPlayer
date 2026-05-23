#include "video_renderer/decode/frame_converter.h"
#include "video_renderer/decode/frame_color_metadata.h"
#include "video_renderer/decode/software_frame_packer.h"
#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace vr {

FrameConverter::FrameConverter() {}

FrameConverter::~FrameConverter() = default;

D3D11SnapshotPoolStats FrameConverter::snapshot_pool_stats() const {
    return hardware_converter_ ? hardware_converter_->snapshot_pool_stats()
                               : D3D11SnapshotPoolStats{};
}

bool FrameConverter::init_software(int src_width, int src_height, AVPixelFormat src_format) {
    width_ = src_width;
    height_ = src_height;
    src_format_ = src_format;
    hardware_converter_.reset();

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
    width_ = src_width;
    height_ = src_height;

    auto hardware_converter = std::make_unique<HardwareFrameConverter>();
    if (!hardware_converter->init(
            d3d_device,
            d3d_context,
            src_width,
            src_height,
            hw_type,
            download_to_cpu,
            device_mutex)) {
        return false;
    }
    hardware_converter_ = std::move(hardware_converter);

    spdlog::info("[FrameConverter] Hardware converter initialized ({}x{}, hw_type={}, download_to_cpu={})",
                 src_width, src_height,
                 hw_decode_type_name(hw_type),
                 download_to_cpu);
    return true;
}

std::optional<TextureFrame> FrameConverter::convert(AVFrame* frame) {
    if (!frame) {
        spdlog::error("[FrameConverter] convert called with null AVFrame");
        return std::nullopt;
    }

    if (hardware_converter_) {
        return hardware_converter_->convert(frame);
    }

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

    // Software 8-bit 4:2:0 can be uploaded as its original Y/U/V planes.
    // Other supported software formats still use the deterministic packer.
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

    return result;
}

std::optional<TextureFrame> FrameConverter::snapshot_hardware_frame(AVFrame* frame) {
    if (!hardware_converter_) {
        return std::nullopt;
    }
    return hardware_converter_->snapshot_frame(frame);
}

} // namespace vr
