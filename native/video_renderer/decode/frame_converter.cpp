#include "video_renderer/decode/frame_converter.h"
#include <spdlog/spdlog.h>
#include <algorithm>
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
constexpr size_t kMaxCpuFrameBytes = size_t{1024} * 1024 * 1024;

bool calculate_yuv420_layout(int width, int height, int bytes_per_component,
                             size_t& y_stride, size_t& uv_stride, size_t& bytes) {
    y_stride = 0;
    uv_stride = 0;
    bytes = 0;
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (width > kMaxDecodedDimension || height > kMaxDecodedDimension) {
        return false;
    }
    if ((width & 1) != 0 || (height & 1) != 0) {
        return false;
    }
    if (bytes_per_component != 1 && bytes_per_component != 2) {
        return false;
    }
    const size_t width_size = static_cast<size_t>(width) *
        static_cast<size_t>(bytes_per_component);
    const size_t height_size = static_cast<size_t>(height);
    y_stride = width_size;
    uv_stride = width_size;
    if (height_size > std::numeric_limits<size_t>::max() / y_stride) {
        return false;
    }
    const size_t y_bytes = y_stride * height_size;
    const size_t uv_height = height_size / 2;
    if (uv_height > 0 &&
        uv_stride > (std::numeric_limits<size_t>::max() - y_bytes) / uv_height) {
        return false;
    }
    bytes = y_bytes + uv_stride * uv_height;
    if (bytes == 0 || bytes > kMaxCpuFrameBytes ||
        y_stride > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        uv_stride > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return true;
}

std::shared_ptr<std::vector<uint8_t>> allocate_cpu_frame_buffer(size_t bytes,
                                                                const char* context) {
    try {
        return std::make_shared<std::vector<uint8_t>>(bytes);
    } catch (const std::bad_alloc&) {
        spdlog::error("[FrameConverter] Failed to allocate {} frame buffer ({} bytes)",
                      context, bytes);
    }
    return nullptr;
}

bool supported_software_format(AVPixelFormat format) {
    // Keep this list deliberately small. FrameConverter is a deterministic
    // pack/upload step, not a general color scaler; adding libswscale/libyuv
    // fallback would risk soft/hard decode color divergence for the same clip.
    switch (format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21:
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUV422P10LE:
    case AV_PIX_FMT_YUV444P10LE:
        return true;
    default:
        return false;
    }
}

bool software_format_uses_p010(AVPixelFormat format) {
    switch (format) {
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_YUV422P10LE:
    case AV_PIX_FMT_YUV444P10LE:
        return true;
    default:
        return false;
    }
}

uint16_t clamp_10(uint16_t value) {
    return std::min<uint16_t>(value, 1023u);
}

uint16_t yuv10_to_p010(uint16_t value) {
    return static_cast<uint16_t>(clamp_10(value) << 6);
}

uint8_t avg2_u8(uint8_t a, uint8_t b) {
    return static_cast<uint8_t>((static_cast<unsigned>(a) + b + 1u) / 2u);
}

uint8_t avg4_u8(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return static_cast<uint8_t>((static_cast<unsigned>(a) + b + c + d + 2u) / 4u);
}

uint16_t avg2_u16(uint16_t a, uint16_t b) {
    return static_cast<uint16_t>((static_cast<unsigned>(a) + b + 1u) / 2u);
}

uint16_t avg4_u16(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    return static_cast<uint16_t>((static_cast<unsigned>(a) + b + c + d + 2u) / 4u);
}

bool copy_nv12_8(const AVFrame* frame, std::vector<uint8_t>& dst,
                 int width, int height, int y_stride, int uv_stride) {
    if (!frame->data[0] || !frame->data[1] ||
        frame->linesize[0] < width || frame->linesize[1] < width) {
        return false;
    }
    uint8_t* dst_y = dst.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(y_stride) * height;
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst_y + static_cast<size_t>(y) * y_stride,
                    frame->data[0] + static_cast<size_t>(y) * frame->linesize[0],
                    static_cast<size_t>(width));
    }
    for (int y = 0; y < height / 2; ++y) {
        std::memcpy(dst_uv + static_cast<size_t>(y) * uv_stride,
                    frame->data[1] + static_cast<size_t>(y) * frame->linesize[1],
                    static_cast<size_t>(width));
    }
    return true;
}

bool pack_yuv420p_8_to_nv12(const AVFrame* frame, std::vector<uint8_t>& dst,
                            int width, int height, int y_stride, int uv_stride) {
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width ||
        frame->linesize[1] < width / 2 ||
        frame->linesize[2] < width / 2) {
        return false;
    }
    uint8_t* dst_y = dst.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(y_stride) * height;
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst_y + static_cast<size_t>(y) * y_stride,
                    frame->data[0] + static_cast<size_t>(y) * frame->linesize[0],
                    static_cast<size_t>(width));
    }
    for (int y = 0; y < height / 2; ++y) {
        const uint8_t* src_u = frame->data[1] + static_cast<size_t>(y) * frame->linesize[1];
        const uint8_t* src_v = frame->data[2] + static_cast<size_t>(y) * frame->linesize[2];
        uint8_t* row = dst_uv + static_cast<size_t>(y) * uv_stride;
        for (int x = 0; x < width / 2; ++x) {
            row[x * 2] = src_u[x];
            row[x * 2 + 1] = src_v[x];
        }
    }
    return true;
}

bool copy_nv21_8_as_nv12(const AVFrame* frame, std::vector<uint8_t>& dst,
                         int width, int height, int y_stride, int uv_stride) {
    if (!frame->data[0] || !frame->data[1] ||
        frame->linesize[0] < width || frame->linesize[1] < width) {
        return false;
    }
    uint8_t* dst_y = dst.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(y_stride) * height;
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst_y + static_cast<size_t>(y) * y_stride,
                    frame->data[0] + static_cast<size_t>(y) * frame->linesize[0],
                    static_cast<size_t>(width));
    }
    for (int y = 0; y < height / 2; ++y) {
        const uint8_t* src_vu = frame->data[1] + static_cast<size_t>(y) * frame->linesize[1];
        uint8_t* row = dst_uv + static_cast<size_t>(y) * uv_stride;
        for (int x = 0; x < width; x += 2) {
            row[x] = src_vu[x + 1];
            row[x + 1] = src_vu[x];
        }
    }
    return true;
}

bool pack_yuv422p_8_to_nv12(const AVFrame* frame, std::vector<uint8_t>& dst,
                            int width, int height, int y_stride, int uv_stride) {
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width ||
        frame->linesize[1] < width / 2 ||
        frame->linesize[2] < width / 2) {
        return false;
    }
    uint8_t* dst_y = dst.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(y_stride) * height;
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst_y + static_cast<size_t>(y) * y_stride,
                    frame->data[0] + static_cast<size_t>(y) * frame->linesize[0],
                    static_cast<size_t>(width));
    }
    for (int y = 0; y < height / 2; ++y) {
        const uint8_t* src_u0 = frame->data[1] + static_cast<size_t>(y * 2) * frame->linesize[1];
        const uint8_t* src_u1 = frame->data[1] + static_cast<size_t>(y * 2 + 1) * frame->linesize[1];
        const uint8_t* src_v0 = frame->data[2] + static_cast<size_t>(y * 2) * frame->linesize[2];
        const uint8_t* src_v1 = frame->data[2] + static_cast<size_t>(y * 2 + 1) * frame->linesize[2];
        uint8_t* row = dst_uv + static_cast<size_t>(y) * uv_stride;
        for (int x = 0; x < width / 2; ++x) {
            row[x * 2] = avg2_u8(src_u0[x], src_u1[x]);
            row[x * 2 + 1] = avg2_u8(src_v0[x], src_v1[x]);
        }
    }
    return true;
}

bool pack_yuv444p_8_to_nv12(const AVFrame* frame, std::vector<uint8_t>& dst,
                            int width, int height, int y_stride, int uv_stride) {
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width ||
        frame->linesize[1] < width ||
        frame->linesize[2] < width) {
        return false;
    }
    uint8_t* dst_y = dst.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(y_stride) * height;
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst_y + static_cast<size_t>(y) * y_stride,
                    frame->data[0] + static_cast<size_t>(y) * frame->linesize[0],
                    static_cast<size_t>(width));
    }
    for (int y = 0; y < height / 2; ++y) {
        const uint8_t* src_u0 = frame->data[1] + static_cast<size_t>(y * 2) * frame->linesize[1];
        const uint8_t* src_u1 = frame->data[1] + static_cast<size_t>(y * 2 + 1) * frame->linesize[1];
        const uint8_t* src_v0 = frame->data[2] + static_cast<size_t>(y * 2) * frame->linesize[2];
        const uint8_t* src_v1 = frame->data[2] + static_cast<size_t>(y * 2 + 1) * frame->linesize[2];
        uint8_t* row = dst_uv + static_cast<size_t>(y) * uv_stride;
        for (int x = 0; x < width / 2; ++x) {
            const int sx = x * 2;
            row[x * 2] = avg4_u8(src_u0[sx], src_u0[sx + 1], src_u1[sx], src_u1[sx + 1]);
            row[x * 2 + 1] = avg4_u8(src_v0[sx], src_v0[sx + 1], src_v1[sx], src_v1[sx + 1]);
        }
    }
    return true;
}

bool pack_yuv420p_10_to_p010(const AVFrame* frame, std::vector<uint8_t>& dst,
                             int width, int height, int y_stride, int uv_stride) {
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width * 2 ||
        frame->linesize[1] < width ||
        frame->linesize[2] < width) {
        return false;
    }
    auto* dst_y = reinterpret_cast<uint16_t*>(dst.data());
    auto* dst_uv = reinterpret_cast<uint16_t*>(dst.data() + static_cast<size_t>(y_stride) * height);
    for (int y = 0; y < height; ++y) {
        const auto* src_y = reinterpret_cast<const uint16_t*>(
            frame->data[0] + static_cast<size_t>(y) * frame->linesize[0]);
        auto* row = reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(dst_y) + static_cast<size_t>(y) * y_stride);
        for (int x = 0; x < width; ++x) {
            row[x] = yuv10_to_p010(src_y[x]);
        }
    }
    for (int y = 0; y < height / 2; ++y) {
        const auto* src_u = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(y) * frame->linesize[1]);
        const auto* src_v = reinterpret_cast<const uint16_t*>(
            frame->data[2] + static_cast<size_t>(y) * frame->linesize[2]);
        auto* row = reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(dst_uv) + static_cast<size_t>(y) * uv_stride);
        for (int x = 0; x < width / 2; ++x) {
            row[x * 2] = yuv10_to_p010(src_u[x]);
            row[x * 2 + 1] = yuv10_to_p010(src_v[x]);
        }
    }
    return true;
}

bool copy_p010(const AVFrame* frame, std::vector<uint8_t>& dst,
               int width, int height, int y_stride, int uv_stride) {
    if (!frame->data[0] || !frame->data[1] ||
        frame->linesize[0] < width * 2 ||
        frame->linesize[1] < width * 2) {
        return false;
    }
    uint8_t* dst_y = dst.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(y_stride) * height;
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst_y + static_cast<size_t>(y) * y_stride,
                    frame->data[0] + static_cast<size_t>(y) * frame->linesize[0],
                    static_cast<size_t>(width) * 2);
    }
    for (int y = 0; y < height / 2; ++y) {
        std::memcpy(dst_uv + static_cast<size_t>(y) * uv_stride,
                    frame->data[1] + static_cast<size_t>(y) * frame->linesize[1],
                    static_cast<size_t>(width) * 2);
    }
    return true;
}

bool pack_yuv422p_10_to_p010(const AVFrame* frame, std::vector<uint8_t>& dst,
                             int width, int height, int y_stride, int uv_stride) {
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width * 2 ||
        frame->linesize[1] < width ||
        frame->linesize[2] < width) {
        return false;
    }
    for (int y = 0; y < height; ++y) {
        const auto* src_y = reinterpret_cast<const uint16_t*>(
            frame->data[0] + static_cast<size_t>(y) * frame->linesize[0]);
        auto* row = reinterpret_cast<uint16_t*>(
            dst.data() + static_cast<size_t>(y) * y_stride);
        for (int x = 0; x < width; ++x) {
            row[x] = yuv10_to_p010(src_y[x]);
        }
    }
    uint8_t* dst_uv = dst.data() + static_cast<size_t>(y_stride) * height;
    for (int y = 0; y < height / 2; ++y) {
        const auto* src_u0 = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(y * 2) * frame->linesize[1]);
        const auto* src_u1 = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(y * 2 + 1) * frame->linesize[1]);
        const auto* src_v0 = reinterpret_cast<const uint16_t*>(
            frame->data[2] + static_cast<size_t>(y * 2) * frame->linesize[2]);
        const auto* src_v1 = reinterpret_cast<const uint16_t*>(
            frame->data[2] + static_cast<size_t>(y * 2 + 1) * frame->linesize[2]);
        auto* row = reinterpret_cast<uint16_t*>(dst_uv + static_cast<size_t>(y) * uv_stride);
        for (int x = 0; x < width / 2; ++x) {
            row[x * 2] = yuv10_to_p010(avg2_u16(src_u0[x], src_u1[x]));
            row[x * 2 + 1] = yuv10_to_p010(avg2_u16(src_v0[x], src_v1[x]));
        }
    }
    return true;
}

bool pack_yuv444p_10_to_p010(const AVFrame* frame, std::vector<uint8_t>& dst,
                             int width, int height, int y_stride, int uv_stride) {
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width * 2 ||
        frame->linesize[1] < width * 2 ||
        frame->linesize[2] < width * 2) {
        return false;
    }
    for (int y = 0; y < height; ++y) {
        const auto* src_y = reinterpret_cast<const uint16_t*>(
            frame->data[0] + static_cast<size_t>(y) * frame->linesize[0]);
        auto* row = reinterpret_cast<uint16_t*>(
            dst.data() + static_cast<size_t>(y) * y_stride);
        for (int x = 0; x < width; ++x) {
            row[x] = yuv10_to_p010(src_y[x]);
        }
    }
    uint8_t* dst_uv = dst.data() + static_cast<size_t>(y_stride) * height;
    for (int y = 0; y < height / 2; ++y) {
        const auto* src_u0 = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(y * 2) * frame->linesize[1]);
        const auto* src_u1 = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(y * 2 + 1) * frame->linesize[1]);
        const auto* src_v0 = reinterpret_cast<const uint16_t*>(
            frame->data[2] + static_cast<size_t>(y * 2) * frame->linesize[2]);
        const auto* src_v1 = reinterpret_cast<const uint16_t*>(
            frame->data[2] + static_cast<size_t>(y * 2 + 1) * frame->linesize[2]);
        auto* row = reinterpret_cast<uint16_t*>(dst_uv + static_cast<size_t>(y) * uv_stride);
        for (int x = 0; x < width / 2; ++x) {
            const int sx = x * 2;
            row[x * 2] = yuv10_to_p010(avg4_u16(src_u0[sx], src_u0[sx + 1],
                                                src_u1[sx], src_u1[sx + 1]));
            row[x * 2 + 1] = yuv10_to_p010(avg4_u16(src_v0[sx], src_v0[sx + 1],
                                                    src_v1[sx], src_v1[sx + 1]));
        }
    }
    return true;
}

bool convert_frame_to_cpu_nv12(const AVFrame* frame,
                               const char* context,
                               TextureFrame& result) {
    if (!frame) {
        return false;
    }
    const int width = frame->width;
    const int height = frame->height;
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const bool use_p010 = software_format_uses_p010(format);
    size_t y_stride = 0;
    size_t uv_stride = 0;
    size_t bytes = 0;
    if (!calculate_yuv420_layout(width, height, use_p010 ? 2 : 1,
                                 y_stride, uv_stride, bytes)) {
        spdlog::error("[FrameConverter] Invalid CPU NV12 layout ({}x{}, format={})",
                      width, height, static_cast<int>(format));
        return false;
    }
    if (!supported_software_format(format)) {
        const char* name = av_get_pix_fmt_name(format);
        spdlog::error("[FrameConverter] Unsupported software pixel format {} ({}) "
                      "without libswscale",
                      static_cast<int>(format), name ? name : "unknown");
        return false;
    }

    auto buffer = allocate_cpu_frame_buffer(bytes, context);
    if (!buffer || buffer->empty()) {
        return false;
    }

    bool ok = false;
    switch (format) {
    case AV_PIX_FMT_NV12:
        ok = copy_nv12_8(frame, *buffer, width, height,
                         static_cast<int>(y_stride), static_cast<int>(uv_stride));
        break;
    case AV_PIX_FMT_NV21:
        ok = copy_nv21_8_as_nv12(frame, *buffer, width, height,
                                 static_cast<int>(y_stride), static_cast<int>(uv_stride));
        break;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        ok = pack_yuv420p_8_to_nv12(frame, *buffer, width, height,
                                    static_cast<int>(y_stride), static_cast<int>(uv_stride));
        break;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
        ok = pack_yuv422p_8_to_nv12(frame, *buffer, width, height,
                                    static_cast<int>(y_stride), static_cast<int>(uv_stride));
        break;
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P:
        ok = pack_yuv444p_8_to_nv12(frame, *buffer, width, height,
                                    static_cast<int>(y_stride), static_cast<int>(uv_stride));
        break;
    case AV_PIX_FMT_YUV420P10LE:
        ok = pack_yuv420p_10_to_p010(frame, *buffer, width, height,
                                     static_cast<int>(y_stride), static_cast<int>(uv_stride));
        break;
    case AV_PIX_FMT_P010LE:
        ok = copy_p010(frame, *buffer, width, height,
                       static_cast<int>(y_stride), static_cast<int>(uv_stride));
        break;
    case AV_PIX_FMT_YUV422P10LE:
        ok = pack_yuv422p_10_to_p010(frame, *buffer, width, height,
                                     static_cast<int>(y_stride), static_cast<int>(uv_stride));
        break;
    case AV_PIX_FMT_YUV444P10LE:
        ok = pack_yuv444p_10_to_p010(frame, *buffer, width, height,
                                     static_cast<int>(y_stride), static_cast<int>(uv_stride));
        break;
    default:
        break;
    }
    if (!ok) {
        spdlog::error("[FrameConverter] Failed to pack {} frame to CPU NV12 ({}x{}, format={})",
                      context, width, height, static_cast<int>(format));
        return false;
    }

    result.width = width;
    result.height = height;
    result.cpu_data = buffer;
    result.texture_handle = buffer->data();
    result.is_ref = false;
    result.is_nv12 = true;
    result.is_p010 = use_p010;
    result.storage = CpuNv12FrameStorage{
        buffer,
        static_cast<int>(y_stride),
        static_cast<int>(uv_stride),
        use_p010,
    };
    return true;
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

bool same_snapshot_desc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) {
    return a.Width == b.Width &&
           a.Height == b.Height &&
           a.MipLevels == b.MipLevels &&
           a.ArraySize == b.ArraySize &&
           a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality &&
           a.Usage == b.Usage &&
           a.BindFlags == b.BindFlags &&
           a.CPUAccessFlags == b.CPUAccessFlags &&
           a.MiscFlags == b.MiscFlags;
}

bool d3d11_surface_is_supported_yuv420(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_NV12 ||
           format == DXGI_FORMAT_P010 ||
           format == DXGI_FORMAT_P016;
}

struct D3D11SnapshotFrameRef {
    ~D3D11SnapshotFrameRef();

    std::weak_ptr<D3D11SnapshotPool> pool;
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

struct D3D11SnapshotPool {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> acquire(
        ID3D11Device* device,
        const D3D11_TEXTURE2D_DESC& desc) {
        if (!device) {
            return {};
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto it = available.begin(); it != available.end(); ++it) {
                if (!*it) {
                    it = available.erase(it);
                    if (it == available.end()) break;
                    continue;
                }
                D3D11_TEXTURE2D_DESC existing_desc = {};
                (*it)->GetDesc(&existing_desc);
                if (!same_snapshot_desc(existing_desc, desc)) {
                    continue;
                }

                Microsoft::WRL::ComPtr<ID3D11Texture2D> texture = *it;
                available.erase(it);
                ++reused_count;
                return texture;
            }
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
        if (FAILED(hr) || !texture) {
            spdlog::warn("[FrameConverter] Failed to create D3D11 exact-seek snapshot: {:#x}",
                         static_cast<unsigned long>(hr));
            return {};
        }

        std::lock_guard<std::mutex> lock(mutex);
        ++created_count;
        return texture;
    }

    void release(Microsoft::WRL::ComPtr<ID3D11Texture2D> texture) {
        if (!texture) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (available.size() >= kMaxAvailable) {
            return;
        }
        available.push_back(std::move(texture));
    }

    static constexpr size_t kMaxAvailable = 4;
    std::mutex mutex;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> available;
    uint64_t created_count = 0;
    uint64_t reused_count = 0;
};

D3D11SnapshotFrameRef::~D3D11SnapshotFrameRef() {
    if (auto owner = pool.lock()) {
        owner->release(texture);
    }
}

FrameConverter::FrameConverter()
{}

FrameConverter::~FrameConverter() = default;

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

    size_t y_stride = 0;
    size_t uv_stride = 0;
    size_t bytes = 0;
    if (!calculate_yuv420_layout(src_width, src_height,
                                 software_format_uses_p010(src_format) ? 2 : 1,
                                 y_stride, uv_stride, bytes)) {
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

    spdlog::info("[FrameConverter] Software converter initialized for CPU NV12 upload "
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
    d3d11_snapshot_pool_ =
        (!download_to_cpu && hw_type == HwDecodeType::D3D11VA)
        ? std::make_shared<D3D11SnapshotPool>()
        : nullptr;

    spdlog::info("[FrameConverter] Hardware converter initialized ({}x{}, hw_type={}, download_to_cpu={})",
                 src_width, src_height,
                 hw_type == HwDecodeType::D3D11VA ? "D3D11VA" : "unknown",
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
        // frame->data[0] = ID3D11Texture2D*, frame->data[1] = array index (intptr_t)
        if (frame->data[0]) {
            result.texture_handle = frame->data[0];
            result.is_ref = true;

            if (hw_type_ == HwDecodeType::D3D11VA) {
                auto* texture = static_cast<ID3D11Texture2D*>(result.texture_handle);
                D3D11_TEXTURE2D_DESC desc = {};
                texture->GetDesc(&desc);
                if (!d3d11_surface_is_supported_yuv420(desc.Format)) {
                    spdlog::error("[FrameConverter] Unsupported D3D11VA surface format {}. "
                                  "Renderer-owned hardware path only supports NV12/P010/P016; "
                                  "use software decode until 4:2:2/4:4:4 GPU shader paths exist.",
                                  static_cast<int>(desc.Format));
                    return std::nullopt;
                }
                result.is_nv12 = true;
                result.is_p010 = desc.Format == DXGI_FORMAT_P010 ||
                    desc.Format == DXGI_FORMAT_P016;
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
        if (!result.texture_handle) {
            spdlog::error("[FrameConverter] Hardware frame has no D3D11 texture");
            return std::nullopt;
        }
    } else {
        // Software path: keep the frame in YUV and upload it as NV12 so it
        // shares the same shader conversion path as D3D11VA decode.
        result.color = color_info_from_frame(frame);
        if (!convert_frame_to_cpu_nv12(frame, "software", result)) {
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
    if (!d3d11_surface_is_supported_yuv420(source_desc.Format)) {
        spdlog::warn("[FrameConverter] Cannot snapshot unsupported D3D11VA surface format {}",
                     static_cast<int>(source_desc.Format));
        return std::nullopt;
    }
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

    if (!d3d11_snapshot_pool_) {
        d3d11_snapshot_pool_ = std::make_shared<D3D11SnapshotPool>();
    }
    Microsoft::WRL::ComPtr<ID3D11Texture2D> snapshot =
        d3d11_snapshot_pool_->acquire(device.Get(), snapshot_desc);
    if (!snapshot) {
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
    snapshot_ref->pool = d3d11_snapshot_pool_;
    snapshot_ref->texture = snapshot;

    TextureFrame result;
    result.pts_us = frame->pts;
    result.dts_us = frame->pkt_dts != AV_NOPTS_VALUE
        ? frame->pkt_dts
        : kNoTimestampUs;
    result.duration_us = frame->duration;
    result.width = frame->width;
    result.height = frame->height;
    result.is_ref = true;
    result.texture_handle = snapshot.Get();
    result.is_nv12 = true;
    result.is_p010 = source_desc.Format == DXGI_FORMAT_P010 ||
        source_desc.Format == DXGI_FORMAT_P016;
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
