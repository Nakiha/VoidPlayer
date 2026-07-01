#include "renderer/decode/software_frame_packer.h"
#include "renderer/decode/av_frame_lifetime.h"
#include "renderer/renderer_limits.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace vr {

namespace {
int ceil_div2(int value) {
    return (value + 1) / 2;
}

int round_up_even(int value) {
    return (value + 1) & ~1;
}

bool calculate_yuv420_layout(int width, int height, int bytes_per_component,
                             size_t& y_stride, size_t& uv_stride, size_t& bytes) {
    y_stride = 0;
    uv_stride = 0;
    bytes = 0;
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (width > kMaxRendererDimension || height > kMaxRendererDimension) {
        return false;
    }
    if (bytes_per_component != 1 && bytes_per_component != 2) {
        return false;
    }
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const size_t width_size = static_cast<size_t>(coded_width) *
        static_cast<size_t>(bytes_per_component);
    const size_t height_size = static_cast<size_t>(coded_height);
    y_stride = width_size;
    uv_stride = width_size;
    if (height_size > std::numeric_limits<size_t>::max() / y_stride) {
        return false;
    }
    const size_t y_bytes = y_stride * height_size;
    const size_t uv_height = static_cast<size_t>(coded_height / 2);
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
} // namespace

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
    case AV_PIX_FMT_YUV420P12LE:
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P012LE:
    case AV_PIX_FMT_P016LE:
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
    case AV_PIX_FMT_YUV420P12LE:
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P012LE:
    case AV_PIX_FMT_P016LE:
    case AV_PIX_FMT_YUV422P10LE:
    case AV_PIX_FMT_YUV444P10LE:
        return true;
    default:
        return false;
    }
}

bool software_format_uses_direct_planar_yuv420(AVPixelFormat format) {
    switch (format) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return true;
    default:
        return false;
    }
}

bool validate_software_frame_layout(int width, int height, AVPixelFormat format) {
    size_t y_stride = 0;
    size_t uv_stride = 0;
    size_t bytes = 0;
    return calculate_yuv420_layout(width, height,
                                   software_format_uses_p010(format) ? 2 : 1,
                                   y_stride, uv_stride, bytes);
}

namespace {
uint16_t clamp_10(uint16_t value) {
    return std::min<uint16_t>(value, 1023u);
}

uint16_t yuv10_to_p010(uint16_t value) {
    return static_cast<uint16_t>(clamp_10(value) << 6);
}

uint16_t yuv12_to_p010(uint16_t value) {
    const uint16_t quantized = static_cast<uint16_t>(
        std::min<unsigned>((static_cast<unsigned>(value) + 2u) >> 2, 1023u));
    return static_cast<uint16_t>(quantized << 6);
}

uint16_t high_bits_to_p010(uint16_t value, int source_bits) {
    if (source_bits <= 10) {
        return yuv10_to_p010(value >> std::max(0, 10 - source_bits));
    }
    const int shift = source_bits - 10;
    const unsigned rounding = shift > 0 ? (1u << (shift - 1)) : 0u;
    const uint16_t quantized = static_cast<uint16_t>(
        std::min<unsigned>((static_cast<unsigned>(value) + rounding) >> shift, 1023u));
    return static_cast<uint16_t>(quantized << 6);
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
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const int chroma_width = ceil_div2(width);
    const int chroma_height = ceil_div2(height);
    if (!frame->data[0] || !frame->data[1] ||
        frame->linesize[0] < width || frame->linesize[1] < chroma_width * 2) {
        return false;
    }
    uint8_t* dst_y = dst.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(y_stride) * coded_height;
    for (int y = 0; y < coded_height; ++y) {
        const int sy = std::min(y, height - 1);
        uint8_t* row = dst_y + static_cast<size_t>(y) * y_stride;
        std::memcpy(row,
                    frame->data[0] + static_cast<size_t>(sy) * frame->linesize[0],
                    static_cast<size_t>(width));
        if (coded_width > width) {
            row[width] = row[width - 1];
        }
    }
    for (int y = 0; y < coded_height / 2; ++y) {
        const int sy = std::min(y, chroma_height - 1);
        std::memcpy(dst_uv + static_cast<size_t>(y) * uv_stride,
                    frame->data[1] + static_cast<size_t>(sy) * frame->linesize[1],
                    static_cast<size_t>(coded_width));
    }
    return true;
}

bool pack_yuv420p_8_to_nv12(const AVFrame* frame, std::vector<uint8_t>& dst,
                            int width, int height, int y_stride, int uv_stride) {
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const int chroma_width = ceil_div2(width);
    const int chroma_height = ceil_div2(height);
    const int coded_chroma_width = coded_width / 2;
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width ||
        frame->linesize[1] < chroma_width ||
        frame->linesize[2] < chroma_width) {
        return false;
    }
    uint8_t* dst_y = dst.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(y_stride) * coded_height;
    for (int y = 0; y < coded_height; ++y) {
        const int sy = std::min(y, height - 1);
        uint8_t* row = dst_y + static_cast<size_t>(y) * y_stride;
        std::memcpy(row,
                    frame->data[0] + static_cast<size_t>(sy) * frame->linesize[0],
                    static_cast<size_t>(width));
        if (coded_width > width) {
            row[width] = row[width - 1];
        }
    }
    for (int y = 0; y < coded_height / 2; ++y) {
        const int sy = std::min(y, chroma_height - 1);
        const uint8_t* src_u = frame->data[1] + static_cast<size_t>(sy) * frame->linesize[1];
        const uint8_t* src_v = frame->data[2] + static_cast<size_t>(sy) * frame->linesize[2];
        uint8_t* row = dst_uv + static_cast<size_t>(y) * uv_stride;
        for (int x = 0; x < coded_chroma_width; ++x) {
            const int sx = std::min(x, chroma_width - 1);
            row[x * 2] = src_u[sx];
            row[x * 2 + 1] = src_v[sx];
        }
    }
    return true;
}

bool copy_nv21_8_as_nv12(const AVFrame* frame, std::vector<uint8_t>& dst,
                         int width, int height, int y_stride, int uv_stride) {
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const int chroma_width = ceil_div2(width);
    const int chroma_height = ceil_div2(height);
    if (!frame->data[0] || !frame->data[1] ||
        frame->linesize[0] < width || frame->linesize[1] < chroma_width * 2) {
        return false;
    }
    uint8_t* dst_y = dst.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(y_stride) * coded_height;
    for (int y = 0; y < coded_height; ++y) {
        const int sy = std::min(y, height - 1);
        uint8_t* row = dst_y + static_cast<size_t>(y) * y_stride;
        std::memcpy(row,
                    frame->data[0] + static_cast<size_t>(sy) * frame->linesize[0],
                    static_cast<size_t>(width));
        if (coded_width > width) {
            row[width] = row[width - 1];
        }
    }
    for (int y = 0; y < coded_height / 2; ++y) {
        const int sy = std::min(y, chroma_height - 1);
        const uint8_t* src_vu = frame->data[1] + static_cast<size_t>(sy) * frame->linesize[1];
        uint8_t* row = dst_uv + static_cast<size_t>(y) * uv_stride;
        for (int x = 0; x < coded_width; x += 2) {
            const int sx = std::min(x / 2, chroma_width - 1) * 2;
            row[x] = src_vu[sx + 1];
            row[x + 1] = src_vu[sx];
        }
    }
    return true;
}

bool pack_yuv422p_8_to_nv12(const AVFrame* frame, std::vector<uint8_t>& dst,
                            int width, int height, int y_stride, int uv_stride) {
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const int chroma_width = ceil_div2(width);
    const int coded_chroma_width = coded_width / 2;
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width ||
        frame->linesize[1] < chroma_width ||
        frame->linesize[2] < chroma_width) {
        return false;
    }
    uint8_t* dst_y = dst.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(y_stride) * coded_height;
    for (int y = 0; y < coded_height; ++y) {
        const int sy = std::min(y, height - 1);
        uint8_t* row = dst_y + static_cast<size_t>(y) * y_stride;
        std::memcpy(row,
                    frame->data[0] + static_cast<size_t>(sy) * frame->linesize[0],
                    static_cast<size_t>(width));
        if (coded_width > width) {
            row[width] = row[width - 1];
        }
    }
    for (int y = 0; y < coded_height / 2; ++y) {
        const int sy0 = std::min(y * 2, height - 1);
        const int sy1 = std::min(y * 2 + 1, height - 1);
        const uint8_t* src_u0 = frame->data[1] + static_cast<size_t>(sy0) * frame->linesize[1];
        const uint8_t* src_u1 = frame->data[1] + static_cast<size_t>(sy1) * frame->linesize[1];
        const uint8_t* src_v0 = frame->data[2] + static_cast<size_t>(sy0) * frame->linesize[2];
        const uint8_t* src_v1 = frame->data[2] + static_cast<size_t>(sy1) * frame->linesize[2];
        uint8_t* row = dst_uv + static_cast<size_t>(y) * uv_stride;
        for (int x = 0; x < coded_chroma_width; ++x) {
            const int sx = std::min(x, chroma_width - 1);
            row[x * 2] = avg2_u8(src_u0[sx], src_u1[sx]);
            row[x * 2 + 1] = avg2_u8(src_v0[sx], src_v1[sx]);
        }
    }
    return true;
}

bool pack_yuv444p_8_to_nv12(const AVFrame* frame, std::vector<uint8_t>& dst,
                            int width, int height, int y_stride, int uv_stride) {
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const int coded_chroma_width = coded_width / 2;
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width ||
        frame->linesize[1] < width ||
        frame->linesize[2] < width) {
        return false;
    }
    uint8_t* dst_y = dst.data();
    uint8_t* dst_uv = dst_y + static_cast<size_t>(y_stride) * coded_height;
    for (int y = 0; y < coded_height; ++y) {
        const int sy = std::min(y, height - 1);
        uint8_t* row = dst_y + static_cast<size_t>(y) * y_stride;
        std::memcpy(row,
                    frame->data[0] + static_cast<size_t>(sy) * frame->linesize[0],
                    static_cast<size_t>(width));
        if (coded_width > width) {
            row[width] = row[width - 1];
        }
    }
    for (int y = 0; y < coded_height / 2; ++y) {
        const int sy0 = std::min(y * 2, height - 1);
        const int sy1 = std::min(y * 2 + 1, height - 1);
        const uint8_t* src_u0 = frame->data[1] + static_cast<size_t>(sy0) * frame->linesize[1];
        const uint8_t* src_u1 = frame->data[1] + static_cast<size_t>(sy1) * frame->linesize[1];
        const uint8_t* src_v0 = frame->data[2] + static_cast<size_t>(sy0) * frame->linesize[2];
        const uint8_t* src_v1 = frame->data[2] + static_cast<size_t>(sy1) * frame->linesize[2];
        uint8_t* row = dst_uv + static_cast<size_t>(y) * uv_stride;
        for (int x = 0; x < coded_chroma_width; ++x) {
            const int sx = std::min(x * 2, width - 1);
            const int sx1 = std::min(sx + 1, width - 1);
            row[x * 2] = avg4_u8(src_u0[sx], src_u0[sx1], src_u1[sx], src_u1[sx1]);
            row[x * 2 + 1] = avg4_u8(src_v0[sx], src_v0[sx1], src_v1[sx], src_v1[sx1]);
        }
    }
    return true;
}

bool pack_yuv420p_10_to_p010(const AVFrame* frame, std::vector<uint8_t>& dst,
                             int width, int height, int y_stride, int uv_stride) {
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const int chroma_width = ceil_div2(width);
    const int chroma_height = ceil_div2(height);
    const int coded_chroma_width = coded_width / 2;
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width * 2 ||
        frame->linesize[1] < chroma_width * 2 ||
        frame->linesize[2] < chroma_width * 2) {
        return false;
    }
    auto* dst_y = reinterpret_cast<uint16_t*>(dst.data());
    auto* dst_uv = reinterpret_cast<uint16_t*>(dst.data() + static_cast<size_t>(y_stride) * coded_height);
    for (int y = 0; y < coded_height; ++y) {
        const int sy = std::min(y, height - 1);
        const auto* src_y = reinterpret_cast<const uint16_t*>(
            frame->data[0] + static_cast<size_t>(sy) * frame->linesize[0]);
        auto* row = reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(dst_y) + static_cast<size_t>(y) * y_stride);
        for (int x = 0; x < width; ++x) {
            row[x] = yuv10_to_p010(src_y[x]);
        }
        if (coded_width > width) {
            row[width] = row[width - 1];
        }
    }
    for (int y = 0; y < coded_height / 2; ++y) {
        const int sy = std::min(y, chroma_height - 1);
        const auto* src_u = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(sy) * frame->linesize[1]);
        const auto* src_v = reinterpret_cast<const uint16_t*>(
            frame->data[2] + static_cast<size_t>(sy) * frame->linesize[2]);
        auto* row = reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(dst_uv) + static_cast<size_t>(y) * uv_stride);
        for (int x = 0; x < coded_chroma_width; ++x) {
            const int sx = std::min(x, chroma_width - 1);
            row[x * 2] = yuv10_to_p010(src_u[sx]);
            row[x * 2 + 1] = yuv10_to_p010(src_v[sx]);
        }
    }
    return true;
}

bool pack_yuv420p_12_to_p010(const AVFrame* frame, std::vector<uint8_t>& dst,
                             int width, int height, int y_stride, int uv_stride) {
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const int chroma_width = ceil_div2(width);
    const int chroma_height = ceil_div2(height);
    const int coded_chroma_width = coded_width / 2;
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width * 2 ||
        frame->linesize[1] < chroma_width * 2 ||
        frame->linesize[2] < chroma_width * 2) {
        return false;
    }
    auto* dst_y = reinterpret_cast<uint16_t*>(dst.data());
    auto* dst_uv = reinterpret_cast<uint16_t*>(
        dst.data() + static_cast<size_t>(y_stride) * coded_height);
    for (int y = 0; y < coded_height; ++y) {
        const int sy = std::min(y, height - 1);
        const auto* src_y = reinterpret_cast<const uint16_t*>(
            frame->data[0] + static_cast<size_t>(sy) * frame->linesize[0]);
        auto* row = reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(dst_y) + static_cast<size_t>(y) * y_stride);
        for (int x = 0; x < width; ++x) {
            row[x] = yuv12_to_p010(src_y[x]);
        }
        if (coded_width > width) {
            row[width] = row[width - 1];
        }
    }
    for (int y = 0; y < coded_height / 2; ++y) {
        const int sy = std::min(y, chroma_height - 1);
        const auto* src_u = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(sy) * frame->linesize[1]);
        const auto* src_v = reinterpret_cast<const uint16_t*>(
            frame->data[2] + static_cast<size_t>(sy) * frame->linesize[2]);
        auto* row = reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(dst_uv) + static_cast<size_t>(y) * uv_stride);
        for (int x = 0; x < coded_chroma_width; ++x) {
            const int sx = std::min(x, chroma_width - 1);
            row[x * 2] = yuv12_to_p010(src_u[sx]);
            row[x * 2 + 1] = yuv12_to_p010(src_v[sx]);
        }
    }
    return true;
}

bool copy_p010(const AVFrame* frame, std::vector<uint8_t>& dst,
               int width, int height, int y_stride, int uv_stride) {
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const int chroma_width = ceil_div2(width);
    const int chroma_height = ceil_div2(height);
    const int coded_chroma_width = coded_width / 2;
    if (!frame->data[0] || !frame->data[1] ||
        frame->linesize[0] < width * 2 ||
        frame->linesize[1] < chroma_width * 4) {
        return false;
    }
    auto* dst_y = reinterpret_cast<uint16_t*>(dst.data());
    auto* dst_uv = reinterpret_cast<uint16_t*>(dst.data() + static_cast<size_t>(y_stride) * coded_height);
    for (int y = 0; y < coded_height; ++y) {
        const int sy = std::min(y, height - 1);
        const auto* src_y = reinterpret_cast<const uint16_t*>(
            frame->data[0] + static_cast<size_t>(sy) * frame->linesize[0]);
        auto* row = reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(dst_y) + static_cast<size_t>(y) * y_stride);
        for (int x = 0; x < width; ++x) {
            row[x] = src_y[x];
        }
        if (coded_width > width) {
            row[width] = row[width - 1];
        }
    }
    for (int y = 0; y < coded_height / 2; ++y) {
        const int sy = std::min(y, chroma_height - 1);
        const auto* src_uv = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(sy) * frame->linesize[1]);
        auto* row = reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(dst_uv) + static_cast<size_t>(y) * uv_stride);
        for (int x = 0; x < coded_chroma_width; ++x) {
            const int sx = std::min(x, chroma_width - 1);
            row[x * 2] = src_uv[sx * 2];
            row[x * 2 + 1] = src_uv[sx * 2 + 1];
        }
    }
    return true;
}

bool copy_high_bit_biplanar_as_p010(const AVFrame* frame,
                                    std::vector<uint8_t>& dst,
                                    int width,
                                    int height,
                                    int y_stride,
                                    int uv_stride,
                                    int source_bits) {
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const int chroma_width = ceil_div2(width);
    const int chroma_height = ceil_div2(height);
    const int coded_chroma_width = coded_width / 2;
    if (!frame->data[0] || !frame->data[1] ||
        frame->linesize[0] < width * 2 ||
        frame->linesize[1] < chroma_width * 4) {
        return false;
    }
    auto* dst_y = reinterpret_cast<uint16_t*>(dst.data());
    auto* dst_uv = reinterpret_cast<uint16_t*>(
        dst.data() + static_cast<size_t>(y_stride) * coded_height);
    for (int y = 0; y < coded_height; ++y) {
        const int sy = std::min(y, height - 1);
        const auto* src_y = reinterpret_cast<const uint16_t*>(
            frame->data[0] + static_cast<size_t>(sy) * frame->linesize[0]);
        auto* row = reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(dst_y) + static_cast<size_t>(y) * y_stride);
        for (int x = 0; x < width; ++x) {
            row[x] = high_bits_to_p010(src_y[x], source_bits);
        }
        if (coded_width > width) {
            row[width] = row[width - 1];
        }
    }
    for (int y = 0; y < coded_height / 2; ++y) {
        const int sy = std::min(y, chroma_height - 1);
        const auto* src_uv = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(sy) * frame->linesize[1]);
        auto* row = reinterpret_cast<uint16_t*>(
            reinterpret_cast<uint8_t*>(dst_uv) + static_cast<size_t>(y) * uv_stride);
        for (int x = 0; x < coded_chroma_width; ++x) {
            const int sx = std::min(x, chroma_width - 1);
            row[x * 2] = high_bits_to_p010(src_uv[sx * 2], source_bits);
            row[x * 2 + 1] = high_bits_to_p010(src_uv[sx * 2 + 1], source_bits);
        }
    }
    return true;
}

bool pack_yuv422p_10_to_p010(const AVFrame* frame, std::vector<uint8_t>& dst,
                             int width, int height, int y_stride, int uv_stride) {
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const int chroma_width = ceil_div2(width);
    const int coded_chroma_width = coded_width / 2;
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width * 2 ||
        frame->linesize[1] < chroma_width * 2 ||
        frame->linesize[2] < chroma_width * 2) {
        return false;
    }
    for (int y = 0; y < coded_height; ++y) {
        const int sy = std::min(y, height - 1);
        const auto* src_y = reinterpret_cast<const uint16_t*>(
            frame->data[0] + static_cast<size_t>(sy) * frame->linesize[0]);
        auto* row = reinterpret_cast<uint16_t*>(
            dst.data() + static_cast<size_t>(y) * y_stride);
        for (int x = 0; x < width; ++x) {
            row[x] = yuv10_to_p010(src_y[x]);
        }
        if (coded_width > width) {
            row[width] = row[width - 1];
        }
    }
    uint8_t* dst_uv = dst.data() + static_cast<size_t>(y_stride) * coded_height;
    for (int y = 0; y < coded_height / 2; ++y) {
        const int sy0 = std::min(y * 2, height - 1);
        const int sy1 = std::min(y * 2 + 1, height - 1);
        const auto* src_u0 = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(sy0) * frame->linesize[1]);
        const auto* src_u1 = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(sy1) * frame->linesize[1]);
        const auto* src_v0 = reinterpret_cast<const uint16_t*>(
            frame->data[2] + static_cast<size_t>(sy0) * frame->linesize[2]);
        const auto* src_v1 = reinterpret_cast<const uint16_t*>(
            frame->data[2] + static_cast<size_t>(sy1) * frame->linesize[2]);
        auto* row = reinterpret_cast<uint16_t*>(dst_uv + static_cast<size_t>(y) * uv_stride);
        for (int x = 0; x < coded_chroma_width; ++x) {
            const int sx = std::min(x, chroma_width - 1);
            row[x * 2] = yuv10_to_p010(avg2_u16(src_u0[sx], src_u1[sx]));
            row[x * 2 + 1] = yuv10_to_p010(avg2_u16(src_v0[sx], src_v1[sx]));
        }
    }
    return true;
}

bool pack_yuv444p_10_to_p010(const AVFrame* frame, std::vector<uint8_t>& dst,
                             int width, int height, int y_stride, int uv_stride) {
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    const int coded_chroma_width = coded_width / 2;
    if (!frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width * 2 ||
        frame->linesize[1] < width * 2 ||
        frame->linesize[2] < width * 2) {
        return false;
    }
    for (int y = 0; y < coded_height; ++y) {
        const int sy = std::min(y, height - 1);
        const auto* src_y = reinterpret_cast<const uint16_t*>(
            frame->data[0] + static_cast<size_t>(sy) * frame->linesize[0]);
        auto* row = reinterpret_cast<uint16_t*>(
            dst.data() + static_cast<size_t>(y) * y_stride);
        for (int x = 0; x < width; ++x) {
            row[x] = yuv10_to_p010(src_y[x]);
        }
        if (coded_width > width) {
            row[width] = row[width - 1];
        }
    }
    uint8_t* dst_uv = dst.data() + static_cast<size_t>(y_stride) * coded_height;
    for (int y = 0; y < coded_height / 2; ++y) {
        const int sy0 = std::min(y * 2, height - 1);
        const int sy1 = std::min(y * 2 + 1, height - 1);
        const auto* src_u0 = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(sy0) * frame->linesize[1]);
        const auto* src_u1 = reinterpret_cast<const uint16_t*>(
            frame->data[1] + static_cast<size_t>(sy1) * frame->linesize[1]);
        const auto* src_v0 = reinterpret_cast<const uint16_t*>(
            frame->data[2] + static_cast<size_t>(sy0) * frame->linesize[2]);
        const auto* src_v1 = reinterpret_cast<const uint16_t*>(
            frame->data[2] + static_cast<size_t>(sy1) * frame->linesize[2]);
        auto* row = reinterpret_cast<uint16_t*>(dst_uv + static_cast<size_t>(y) * uv_stride);
        for (int x = 0; x < coded_chroma_width; ++x) {
            const int sx = std::min(x * 2, width - 1);
            const int sx1 = std::min(sx + 1, width - 1);
            row[x * 2] = yuv10_to_p010(avg4_u16(src_u0[sx], src_u0[sx1],
                                                src_u1[sx], src_u1[sx1]));
            row[x * 2 + 1] = yuv10_to_p010(avg4_u16(src_v0[sx], src_v0[sx1],
                                                    src_v1[sx], src_v1[sx1]));
        }
    }
    return true;
}
} // namespace

bool wrap_frame_as_cpu_planar_yuv420(const AVFrame* frame,
                                     TextureFrame& result) {
    if (!frame) {
        return false;
    }
    const int width = frame->width;
    const int height = frame->height;
    const auto format = static_cast<AVPixelFormat>(frame->format);
    const int chroma_width = ceil_div2(width);
    const int chroma_height = ceil_div2(height);
    if (!software_format_uses_direct_planar_yuv420(format) ||
        width <= 0 || height <= 0 ||
        width > kMaxRendererDimension || height > kMaxRendererDimension ||
        !frame->data[0] || !frame->data[1] || !frame->data[2] ||
        frame->linesize[0] < width ||
        frame->linesize[1] < chroma_width ||
        frame->linesize[2] < chroma_width) {
        return false;
    }

    auto ref_frame_owner = AvFrameOwner::allocate();
    AVFrame* ref_frame = ref_frame_owner.get();
    if (!ref_frame) {
        spdlog::error("[FrameConverter] Failed to allocate software planar frame ref");
        return false;
    }
    if (av_frame_ref(ref_frame, frame) < 0) {
        spdlog::error("[FrameConverter] Failed to ref software planar frame");
        return false;
    }

    auto frame_ref = std::shared_ptr<void>(ref_frame_owner.release(), [](void* p) {
        AVFrame* f = static_cast<AVFrame*>(p);
        av_frame_free(&f);
    });

    result.width = width;
    result.height = height;
    result.texture_handle = ref_frame->data[0];
    result.is_ref = false;
    result.is_nv12 = false;
    result.is_p010 = false;
    result.storage = CpuPlanarYuvFrameStorage{
        frame_ref,
        {ref_frame->data[0], ref_frame->data[1], ref_frame->data[2]},
        {ref_frame->linesize[0], ref_frame->linesize[1], ref_frame->linesize[2]},
        {width, chroma_width, chroma_width},
        {height, chroma_height, chroma_height},
        1,
    };
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
    case AV_PIX_FMT_YUV420P12LE:
        ok = pack_yuv420p_12_to_p010(frame, *buffer, width, height,
                                     static_cast<int>(y_stride), static_cast<int>(uv_stride));
        break;
    case AV_PIX_FMT_P010LE:
        ok = copy_p010(frame, *buffer, width, height,
                       static_cast<int>(y_stride), static_cast<int>(uv_stride));
        break;
    case AV_PIX_FMT_P012LE:
        ok = copy_high_bit_biplanar_as_p010(frame, *buffer, width, height,
                                            static_cast<int>(y_stride),
                                            static_cast<int>(uv_stride),
                                            12);
        break;
    case AV_PIX_FMT_P016LE:
        ok = copy_high_bit_biplanar_as_p010(frame, *buffer, width, height,
                                            static_cast<int>(y_stride),
                                            static_cast<int>(uv_stride),
                                            16);
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
    const int coded_width = round_up_even(width);
    const int coded_height = round_up_even(height);
    result.storage = CpuNv12FrameStorage{
        buffer,
        static_cast<int>(y_stride),
        static_cast<int>(uv_stride),
        use_p010,
        coded_width,
        coded_height,
    };
    return true;
}

} // namespace vr
