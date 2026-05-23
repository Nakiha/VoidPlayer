#include "video_renderer/decode/frame_converter.h"
#include "video_renderer/decode/software_frame_packer.h"
#include <spdlog/spdlog.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace vr {

namespace {

VideoColorRange map_color_range(AVColorRange range) {
    switch (range) {
    case AVCOL_RANGE_JPEG:
        return VIDEO_COLOR_RANGE_FULL;
    case AVCOL_RANGE_MPEG:
        return VIDEO_COLOR_RANGE_LIMITED;
    default:
        return VIDEO_COLOR_RANGE_UNKNOWN;
    }
}

VideoColorMatrix map_color_matrix(AVColorSpace space) {
    switch (space) {
    case AVCOL_SPC_BT709:
        return VIDEO_COLOR_MATRIX_BT709;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
        return VIDEO_COLOR_MATRIX_BT601;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return VIDEO_COLOR_MATRIX_BT2020_NCL;
    default:
        return VIDEO_COLOR_MATRIX_UNKNOWN;
    }
}

VideoColorTransfer map_color_transfer(AVColorTransferCharacteristic transfer) {
    switch (transfer) {
    case AVCOL_TRC_SMPTE2084:
        return VIDEO_COLOR_TRANSFER_PQ;
    case AVCOL_TRC_ARIB_STD_B67:
        return VIDEO_COLOR_TRANSFER_HLG;
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_GAMMA22:
    case AVCOL_TRC_GAMMA28:
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_SMPTE240M:
    case AVCOL_TRC_IEC61966_2_1:
    case AVCOL_TRC_BT2020_10:
    case AVCOL_TRC_BT2020_12:
        return VIDEO_COLOR_TRANSFER_SDR;
    default:
        return VIDEO_COLOR_TRANSFER_UNKNOWN;
    }
}

VideoColorPrimaries map_color_primaries(AVColorPrimaries primaries) {
    switch (primaries) {
    case AVCOL_PRI_BT709:
        return VIDEO_COLOR_PRIMARIES_BT709;
    case AVCOL_PRI_BT470BG:
    case AVCOL_PRI_SMPTE170M:
    case AVCOL_PRI_SMPTE240M:
        return VIDEO_COLOR_PRIMARIES_BT601;
    case AVCOL_PRI_BT2020:
        return VIDEO_COLOR_PRIMARIES_BT2020;
    default:
        return VIDEO_COLOR_PRIMARIES_UNKNOWN;
    }
}

VideoColorInfo color_info_from_frame(const AVFrame* frame) {
    VideoColorInfo info;
    if (!frame) {
        return info;
    }

    info.range = map_color_range(frame->color_range);
    info.matrix = map_color_matrix(frame->colorspace);
    info.transfer = map_color_transfer(frame->color_trc);
    info.primaries = map_color_primaries(frame->color_primaries);

    // FFmpeg often leaves screen recordings partially unspecified. Pick the
    // same conservative defaults most players use for YUV video.
    const auto format = static_cast<AVPixelFormat>(frame->format);
    if (info.range == VIDEO_COLOR_RANGE_UNKNOWN &&
        (format == AV_PIX_FMT_YUVJ420P ||
         format == AV_PIX_FMT_YUVJ422P ||
         format == AV_PIX_FMT_YUVJ444P)) {
        info.range = VIDEO_COLOR_RANGE_FULL;
    }
    if (info.range == VIDEO_COLOR_RANGE_UNKNOWN) {
        info.range = VIDEO_COLOR_RANGE_LIMITED;
    }
    if (info.matrix == VIDEO_COLOR_MATRIX_UNKNOWN) {
        info.matrix = frame->width >= 1280 || frame->height > 576
            ? VIDEO_COLOR_MATRIX_BT709
            : VIDEO_COLOR_MATRIX_BT601;
    }
    if (info.transfer == VIDEO_COLOR_TRANSFER_UNKNOWN) {
        info.transfer = VIDEO_COLOR_TRANSFER_SDR;
    }
    if (info.primaries == VIDEO_COLOR_PRIMARIES_UNKNOWN) {
        info.primaries = info.matrix == VIDEO_COLOR_MATRIX_BT2020_NCL
            ? VIDEO_COLOR_PRIMARIES_BT2020
            : (info.matrix == VIDEO_COLOR_MATRIX_BT601
                ? VIDEO_COLOR_PRIMARIES_BT601
                : VIDEO_COLOR_PRIMARIES_BT709);
    }
    return info;
}

}  // namespace

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
        ? std::make_shared<D3D11SnapshotPool>()
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
    result.color = color_info_from_frame(frame);

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
        result.color = color_info_from_frame(sw_frame);
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
        result.color = color_info_from_frame(frame);
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
    result.color = color_info_from_frame(frame);
    return snapshot_d3d11_hardware_frame(
        frame,
        result,
        device_mutex_,
        d3d11_snapshot_pool_);
}

} // namespace vr
