#include "video_renderer/decode/frame_converter.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <vector>
#include <d3d11.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace vr {

namespace {
constexpr int kMaxDecodedDimension = 16384;
constexpr size_t kMaxRgbaBytes = size_t{1024} * 1024 * 1024;

bool calculate_rgba_layout(int width, int height, size_t& stride, size_t& bytes) {
    stride = 0;
    bytes = 0;
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (width > kMaxDecodedDimension || height > kMaxDecodedDimension) {
        return false;
    }
    const size_t width_size = static_cast<size_t>(width);
    const size_t height_size = static_cast<size_t>(height);
    if (width_size > std::numeric_limits<size_t>::max() / 4) {
        return false;
    }
    stride = width_size * 4;
    if (stride > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    if (height_size > std::numeric_limits<size_t>::max() / stride) {
        return false;
    }
    bytes = stride * height_size;
    if (bytes == 0 || bytes > kMaxRgbaBytes) {
        return false;
    }
    return true;
}

std::shared_ptr<std::vector<uint8_t>> allocate_rgba_buffer(size_t bytes,
                                                           const char* context) {
    try {
        return std::make_shared<std::vector<uint8_t>>(bytes);
    } catch (const std::bad_alloc&) {
        spdlog::error("[FrameConverter] Failed to allocate {} RGBA buffer ({} bytes)",
                      context, bytes);
    }
    return nullptr;
}

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

int sws_colorspace_for_matrix(int matrix) {
    switch (matrix) {
    case VIDEO_COLOR_MATRIX_BT709:
        return SWS_CS_ITU709;
    case VIDEO_COLOR_MATRIX_BT2020_NCL:
        return SWS_CS_BT2020;
    case VIDEO_COLOR_MATRIX_BT601:
    default:
        return SWS_CS_DEFAULT;
    }
}

struct D3D11SnapshotFrameRef {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
};

void wait_d3d11_context_idle(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!context) {
        return;
    }
    if (!device) {
        context->Flush();
        return;
    }

    D3D11_QUERY_DESC query_desc = {};
    query_desc.Query = D3D11_QUERY_EVENT;
    Microsoft::WRL::ComPtr<ID3D11Query> query;
    HRESULT hr = device->CreateQuery(&query_desc, &query);
    if (FAILED(hr) || !query) {
        context->Flush();
        return;
    }

    context->End(query.Get());
    context->Flush();
    const auto start = std::chrono::steady_clock::now();
    while ((hr = context->GetData(query.Get(), nullptr, 0, 0)) == S_FALSE) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100)) {
            spdlog::warn("[FrameConverter] D3D11 snapshot fence timeout after 100ms");
            break;
        }
    }
    if (FAILED(hr)) {
        spdlog::warn("[FrameConverter] D3D11 snapshot fence GetData failed: {:#x}",
                     static_cast<unsigned long>(hr));
    }
}
}  // namespace

FrameConverter::FrameConverter()
{}

FrameConverter::~FrameConverter() {
    reset_sws_context();
}

void FrameConverter::reset_sws_context() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    sws_src_width_ = 0;
    sws_src_height_ = 0;
    sws_src_format_ = AV_PIX_FMT_NONE;
    sws_color_ = {};
}

bool FrameConverter::ensure_sws_context(int src_width, int src_height, AVPixelFormat src_format,
                                        const VideoColorInfo& color) {
    if (src_width <= 0 || src_height <= 0 || src_format == AV_PIX_FMT_NONE) {
        spdlog::error("[FrameConverter] Invalid SwsContext request ({}x{}, format={})",
                      src_width, src_height, static_cast<int>(src_format));
        return false;
    }
    size_t rgba_stride = 0;
    size_t rgba_bytes = 0;
    if (!calculate_rgba_layout(src_width, src_height, rgba_stride, rgba_bytes)) {
        spdlog::error("[FrameConverter] Refusing unsupported frame geometry ({}x{}, format={})",
                      src_width, src_height, static_cast<int>(src_format));
        return false;
    }

    if (sws_ctx_ &&
        sws_src_width_ == src_width &&
        sws_src_height_ == src_height &&
        sws_src_format_ == src_format &&
        sws_color_.range == color.range &&
        sws_color_.matrix == color.matrix &&
        sws_color_.transfer == color.transfer &&
        sws_color_.primaries == color.primaries) {
        return true;
    }

    reset_sws_context();
    sws_ctx_ = sws_getContext(
        src_width, src_height, src_format,
        src_width, src_height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR,
        nullptr, nullptr, nullptr);

    if (!sws_ctx_) {
        spdlog::error("[FrameConverter] Failed to create SwsContext ({}x{}, format={})",
                      src_width, src_height, static_cast<int>(src_format));
        return false;
    }

    const int* coeffs = sws_getCoefficients(sws_colorspace_for_matrix(color.matrix));
    const int src_range = color.range == VIDEO_COLOR_RANGE_FULL ? 1 : 0;
    const int dst_range = 1;
    if (sws_setColorspaceDetails(
            sws_ctx_,
            coeffs,
            src_range,
            coeffs,
            dst_range,
            0, 1 << 16, 1 << 16) < 0) {
        spdlog::warn("[FrameConverter] Failed to set SwsContext colorspace details");
    }

    sws_src_width_ = src_width;
    sws_src_height_ = src_height;
    sws_src_format_ = src_format;
    sws_color_ = color;
    spdlog::info("[FrameConverter] SwsContext initialized ({}x{}, format={}, range={}, matrix={})",
                 src_width, src_height, static_cast<int>(src_format),
                 color.range, color.matrix);
    return true;
}

bool FrameConverter::init_software(int src_width, int src_height, AVPixelFormat src_format) {
    reset_sws_context();

    width_ = src_width;
    height_ = src_height;
    src_format_ = src_format;
    is_hw_ = false;
    download_hw_to_cpu_ = false;
    hw_type_ = HwDecodeType::None;
    d3d_device_ = nullptr;
    d3d_context_ = nullptr;
    device_mutex_ = nullptr;

    VideoColorInfo default_color;
    default_color.range = VIDEO_COLOR_RANGE_LIMITED;
    default_color.matrix = src_width >= 1280 || src_height > 576
        ? VIDEO_COLOR_MATRIX_BT709
        : VIDEO_COLOR_MATRIX_BT601;
    default_color.transfer = VIDEO_COLOR_TRANSFER_SDR;
    default_color.primaries = default_color.matrix == VIDEO_COLOR_MATRIX_BT601
        ? VIDEO_COLOR_PRIMARIES_BT601
        : VIDEO_COLOR_PRIMARIES_BT709;
    if (!ensure_sws_context(src_width, src_height, src_format, default_color)) {
        return false;
    }

    spdlog::info("[FrameConverter] Software converter initialized ({}x{}, format={})",
                 src_width, src_height, static_cast<int>(src_format));
    return true;
}

bool FrameConverter::init_hardware(void* d3d_device, void* d3d_context,
                                   int src_width, int src_height,
                                   HwDecodeType hw_type,
                                   bool download_to_cpu,
                                   std::recursive_mutex* device_mutex) {
    reset_sws_context();

    d3d_device_ = d3d_device;
    d3d_context_ = d3d_context;
    device_mutex_ = device_mutex;
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
    if (!frame) {
        spdlog::error("[FrameConverter] convert called with null AVFrame");
        return result;
    }

    result.pts_us = frame->pts;
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
        result.color = color_info_from_frame(sw_frame);
        if (!ensure_sws_context(sw_frame->width, sw_frame->height, sw_format, result.color)) {
            av_frame_free(&sw_frame);
            return result;
        }
        downloaded_format_ = sw_format;

        result.width = sw_frame->width;
        result.height = sw_frame->height;
        size_t stride = 0;
        size_t buf_size = 0;
        if (!calculate_rgba_layout(sw_frame->width, sw_frame->height, stride, buf_size)) {
            spdlog::error("[FrameConverter] Invalid hw-download RGBA layout ({}x{})",
                          sw_frame->width, sw_frame->height);
            av_frame_free(&sw_frame);
            return result;
        }
        auto rgba_buf = allocate_rgba_buffer(buf_size, "hw-download");
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
        // Software path: convert to RGBA via sws_scale. Some codecs can
        // legally change frame geometry mid-stream, so key the converter and
        // output buffer off the actual AVFrame rather than constructor stats.
        const int frame_width = frame ? frame->width : 0;
        const int frame_height = frame ? frame->height : 0;
        const auto frame_format = frame
            ? static_cast<AVPixelFormat>(frame->format)
            : AV_PIX_FMT_NONE;
        result.color = color_info_from_frame(frame);
        if (!ensure_sws_context(frame_width, frame_height, frame_format, result.color)) {
            return result;
        }

        width_ = frame_width;
        height_ = frame_height;
        src_format_ = frame_format;

        size_t stride = 0;
        size_t buf_size = 0;
        if (!calculate_rgba_layout(frame_width, frame_height, stride, buf_size)) {
            spdlog::error("[FrameConverter] Invalid RGBA layout ({}x{})",
                          frame_width, frame_height);
            return result;
        }
        auto rgba_buf = allocate_rgba_buffer(buf_size, "software");
        if (!rgba_buf || rgba_buf->empty()) {
            spdlog::error("[FrameConverter] Failed to allocate RGBA buffer ({} bytes)", buf_size);
            return result;
        }

        uint8_t* dst_slices[1] = { rgba_buf->data() };
        int dst_stride[1] = { static_cast<int>(stride) };

        int converted_height = sws_scale(
            sws_ctx_,
            frame->data, frame->linesize,
            0, frame_height,
            dst_slices, dst_stride
        );

        if (converted_height != frame_height) {
            spdlog::warn("[FrameConverter] sws_scale converted {} rows (expected {})",
                         converted_height, frame_height);
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

std::optional<TextureFrame> FrameConverter::snapshot_hardware_frame(AVFrame* frame) {
    if (!is_hw_ || download_hw_to_cpu_ || hw_type_ != HwDecodeType::D3D11VA ||
        !frame || !frame->data[0]) {
        return std::nullopt;
    }

    std::unique_lock<std::recursive_mutex> d3d_lock;
    if (device_mutex_) {
        d3d_lock = std::unique_lock<std::recursive_mutex>(*device_mutex_);
    }

    auto* source = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    const int array_idx = static_cast<int>(reinterpret_cast<intptr_t>(frame->data[1]));
    if (array_idx < 0) {
        return std::nullopt;
    }

    D3D11_TEXTURE2D_DESC source_desc = {};
    source->GetDesc(&source_desc);
    if (static_cast<UINT>(array_idx) >= source_desc.ArraySize) {
        spdlog::warn("[FrameConverter] D3D11 snapshot array index out of range: idx={}, array_size={}",
                     array_idx, source_desc.ArraySize);
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    if (!device) {
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) {
        return std::nullopt;
    }

    D3D11_TEXTURE2D_DESC snapshot_desc = source_desc;
    snapshot_desc.ArraySize = 1;
    snapshot_desc.Usage = D3D11_USAGE_DEFAULT;
    snapshot_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    snapshot_desc.CPUAccessFlags = 0;
    snapshot_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> snapshot;
    HRESULT hr = device->CreateTexture2D(&snapshot_desc, nullptr, &snapshot);
    if (FAILED(hr) || !snapshot) {
        spdlog::warn("[FrameConverter] Failed to create D3D11 exact-seek snapshot: {:#x}",
                     static_cast<unsigned long>(hr));
        return std::nullopt;
    }

    context->CopySubresourceRegion(
        snapshot.Get(),
        0,
        0, 0, 0,
        source,
        D3D11CalcSubresource(0, static_cast<UINT>(array_idx), source_desc.MipLevels),
        nullptr);
    wait_d3d11_context_idle(device.Get(), context.Get());

    auto snapshot_ref = std::make_shared<D3D11SnapshotFrameRef>();
    snapshot_ref->texture = snapshot;

    TextureFrame result;
    result.pts_us = frame->pts;
    result.duration_us = frame->duration;
    result.width = frame->width;
    result.height = frame->height;
    result.is_ref = true;
    result.texture_handle = snapshot.Get();
    result.is_nv12 = true;
    result.texture_array_index = 0;
    result.color = color_info_from_frame(frame);
    result.hw_frame_ref = snapshot_ref;
    result.storage = D3D11Nv12FrameStorage{
        snapshot.Get(),
        0,
        result.hw_frame_ref,
    };
    return result;
}

} // namespace vr
