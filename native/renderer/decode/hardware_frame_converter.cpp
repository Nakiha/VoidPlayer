#include "renderer/decode/hardware_frame_converter.h"

#include "renderer/decode/av_frame_lifetime.h"
#include "renderer/decode/frame_color_metadata.h"
#include "renderer/decode/frame_identity.h"
#include "renderer/decode/software_frame_packer.h"

#include <spdlog/spdlog.h>

#ifdef __APPLE__
#include <CoreVideo/CoreVideo.h>
#endif
#ifdef _WIN32
#include <d3d12.h>
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#ifdef _WIN32
#include <libavutil/hwcontext_d3d12va.h>
#endif
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
    populate_frame_identity_from_av_frame(frame, result);
    return result;
}

std::shared_ptr<void> clone_av_frame_ref(AVFrame* frame) {
    AVFrame* clone = av_frame_clone(frame);
    if (!clone) {
        return {};
    }
    return std::shared_ptr<void>(clone, [](void* ptr) {
        auto* owned = static_cast<AVFrame*>(ptr);
        av_frame_free(&owned);
    });
}

#ifdef __APPLE__
bool cv_pixel_format_is_nv12(uint32_t format) {
    return format == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
           format == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
}

bool cv_pixel_format_is_p010(uint32_t format) {
    return format == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
           format == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange;
}

std::optional<TextureFrame> populate_videotoolbox_frame(AVFrame* frame,
                                                        TextureFrame metadata) {
    auto* pixel_buffer = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);
    if (!pixel_buffer) {
        spdlog::error("[HardwareFrameConverter] VideoToolbox frame is missing CVPixelBuffer");
        return std::nullopt;
    }

    const auto pixel_format =
        static_cast<uint32_t>(CVPixelBufferGetPixelFormatType(pixel_buffer));
    if (!cv_pixel_format_is_nv12(pixel_format) && !cv_pixel_format_is_p010(pixel_format)) {
        spdlog::error("[HardwareFrameConverter] Unsupported VideoToolbox CVPixelBuffer format {}",
                      pixel_format);
        return std::nullopt;
    }

    auto frame_ref = clone_av_frame_ref(frame);
    if (!frame_ref) {
        spdlog::error("[HardwareFrameConverter] Failed to retain VideoToolbox frame");
        return std::nullopt;
    }

    const int plane_count = static_cast<int>(CVPixelBufferGetPlaneCount(pixel_buffer));
    const int coded_width = plane_count > 0
        ? static_cast<int>(CVPixelBufferGetWidthOfPlane(pixel_buffer, 0))
        : static_cast<int>(CVPixelBufferGetWidth(pixel_buffer));
    const int coded_height = plane_count > 0
        ? static_cast<int>(CVPixelBufferGetHeightOfPlane(pixel_buffer, 0))
        : static_cast<int>(CVPixelBufferGetHeight(pixel_buffer));

    metadata.texture_handle = pixel_buffer;
    metadata.is_ref = true;
    metadata.is_nv12 = true;
    metadata.is_p010 = cv_pixel_format_is_p010(pixel_format);
    metadata.hw_frame_ref = frame_ref;
    metadata.storage = MacOSCVPixelBufferFrameStorage{
        pixel_buffer,
        pixel_format,
        plane_count,
        metadata.is_p010,
        coded_width,
        coded_height,
        frame_ref,
    };
    return metadata;
}
#endif

#ifdef _WIN32
bool av_pix_fmt_is_p010_like(AVPixelFormat format) {
    return format == AV_PIX_FMT_P010LE || format == AV_PIX_FMT_P016LE ||
           format == AV_PIX_FMT_YUV420P10LE || format == AV_PIX_FMT_YUV420P12LE;
}

std::optional<TextureFrame> populate_d3d12_hardware_texture_frame(
    AVFrame* frame,
    TextureFrame metadata) {
    if (frame->format != AV_PIX_FMT_D3D12 || !frame->data[0]) {
        spdlog::error("[HardwareFrameConverter] D3D12VA frame is missing AVD3D12VAFrame");
        return std::nullopt;
    }

    auto* d3d12_frame = reinterpret_cast<AVD3D12VAFrame*>(frame->data[0]);
    if (!d3d12_frame->texture) {
        spdlog::error("[HardwareFrameConverter] D3D12VA frame is missing ID3D12Resource");
        return std::nullopt;
    }

    auto frame_ref = clone_av_frame_ref(frame);
    if (!frame_ref) {
        spdlog::error("[HardwareFrameConverter] Failed to retain D3D12VA frame");
        return std::nullopt;
    }

    int coded_width = frame->width;
    int coded_height = frame->height;
    AVPixelFormat sw_format = AV_PIX_FMT_NONE;
    if (frame->hw_frames_ctx) {
        auto* frames_ctx =
            reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
        if (frames_ctx) {
            coded_width = frames_ctx->width > 0 ? frames_ctx->width : coded_width;
            coded_height = frames_ctx->height > 0 ? frames_ctx->height : coded_height;
            sw_format = static_cast<AVPixelFormat>(frames_ctx->sw_format);
        }
    }

    metadata.texture_handle = d3d12_frame->texture;
    metadata.is_ref = true;
    metadata.is_nv12 = true;
    metadata.is_p010 = av_pix_fmt_is_p010_like(sw_format);
    metadata.texture_array_index = d3d12_frame->subresource_index;
    metadata.hw_frame_ref = frame_ref;
    metadata.storage = D3D12TextureFrameStorage{
        d3d12_frame->texture,
        d3d12_frame->subresource_index,
        d3d12_frame->sync_ctx.fence,
        d3d12_frame->sync_ctx.event,
        d3d12_frame->sync_ctx.fence_value,
        (d3d12_frame->flags & AV_D3D12VA_FRAME_FLAG_TEXTURE_ARRAY) != 0,
        metadata.is_p010,
        coded_width,
        coded_height,
        frame_ref,
    };
    return metadata;
}
#endif

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
#ifdef __APPLE__
        if (hw_type != HwDecodeType::VideoToolbox) {
            spdlog::error("[HardwareFrameConverter] Renderer-owned hardware frames require a platform presenter");
            return false;
        }
#else
        spdlog::error("[HardwareFrameConverter] Renderer-owned hardware frames are Windows-only");
        return false;
#endif
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
        auto sw_frame = AvFrameOwner::allocate();
        if (!sw_frame) {
            spdlog::error("[HardwareFrameConverter] Failed to allocate hw download frame");
            return std::nullopt;
        }

        const int ret = av_hwframe_transfer_data(sw_frame.get(), frame, 0);
        if (ret < 0) {
            spdlog::error("[HardwareFrameConverter] av_hwframe_transfer_data failed: {:#x}",
                          static_cast<unsigned>(ret));
            return std::nullopt;
        }

        TextureFrame result = make_texture_frame_metadata(frame);
        result.color = color_info_from_av_frame(sw_frame.get());
        downloaded_format_ = static_cast<AVPixelFormat>(sw_frame.get()->format);
        if (!convert_frame_to_cpu_nv12(sw_frame.get(), "hw-download", result)) {
            return std::nullopt;
        }
        return result;
    }

    TextureFrame result = make_texture_frame_metadata(frame);
    if (hw_type_ == HwDecodeType::D3D11VA) {
#ifdef _WIN32
        if (!populate_d3d11_hardware_texture_frame(frame, result)) {
            return std::nullopt;
        }
        return result;
#else
        spdlog::error("[HardwareFrameConverter] D3D11VA frames are Windows-only");
        return std::nullopt;
#endif
    }

#ifdef _WIN32
    if (hw_type_ == HwDecodeType::D3D12VA) {
        return populate_d3d12_hardware_texture_frame(frame, result);
    }
#endif

#ifdef __APPLE__
    if (hw_type_ == HwDecodeType::VideoToolbox) {
        return populate_videotoolbox_frame(frame, result);
    }
#endif

    spdlog::error("[HardwareFrameConverter] Renderer-owned hardware frames require a platform presenter");
    return std::nullopt;
}

std::optional<TextureFrame> HardwareFrameConverter::snapshot_frame(AVFrame* frame) {
#ifdef _WIN32
    if (download_to_cpu_ || hw_type_ != HwDecodeType::D3D11VA || !frame || !frame->data[0]) {
        return std::nullopt;
    }

    TextureFrame metadata = make_texture_frame_metadata(frame);
    return snapshot_d3d11_hardware_frame(
        frame,
        metadata,
        device_mutex_,
        d3d11_snapshot_pool_);
#else
    (void)frame;
    return std::nullopt;
#endif
}

D3D11SnapshotPoolStats HardwareFrameConverter::snapshot_pool_stats() const {
#ifdef _WIN32
    return d3d11_snapshot_pool_stats(d3d11_snapshot_pool_);
#else
    return {};
#endif
}

} // namespace vr
