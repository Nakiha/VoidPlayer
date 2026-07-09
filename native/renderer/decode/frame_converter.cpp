#include "renderer/decode/frame_converter.h"
#include "renderer/decode/decode_stage_perf.h"
#include "renderer/decode/frame_color_metadata.h"
#include "renderer/decode/frame_identity.h"
#include "renderer/decode/software_frame_packer.h"
#include <spdlog/spdlog.h>
#include <chrono>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace vr {
namespace {

uint64_t elapsed_us_since(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}

} // namespace

FrameConverter::FrameConverter() {}

FrameConverter::~FrameConverter() = default;

HardwareSnapshotPoolStats FrameConverter::snapshot_pool_stats() const {
    return hardware_converter_ ? hardware_converter_->snapshot_pool_stats()
                               : HardwareSnapshotPoolStats{};
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

std::optional<TextureFrame> FrameConverter::convert(AVFrame* frame,
                                                    DecodeStagePerfCounters* stage_perf) {
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
    populate_frame_identity_from_av_frame(frame, result);

    // Software 4:2:0 frames can be uploaded from their original planes. Keep
    // the deterministic packer only for formats that need chroma resampling.
    if (software_format_uses_direct_yuv420_storage(
            static_cast<AVPixelFormat>(frame->format))) {
        const auto direct_start = std::chrono::steady_clock::now();
        if (!wrap_frame_as_cpu_yuv420_storage(frame, result)) {
            return std::nullopt;
        }
        if (stage_perf) {
            stage_perf->record_convert_direct_planar(elapsed_us_since(direct_start));
        }
    } else {
        SoftwareFramePackTiming timing;
        if (!convert_frame_to_cpu_nv12(frame, "software", result, &timing)) {
            return std::nullopt;
        }
        if (stage_perf) {
            stage_perf->record_convert_nv12_layout(timing.layout_us);
            stage_perf->record_convert_nv12_alloc(timing.alloc_us);
            stage_perf->record_convert_nv12_pack(timing.pack_us);
        }
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
    if (hardware_converter_->hw_type() == HwDecodeType::D3D12VA ||
        hardware_converter_->hw_type() == HwDecodeType::VideoToolbox) {
        return hardware_converter_->convert(frame);
    }
    return hardware_converter_->snapshot_frame(frame);
}

} // namespace vr
