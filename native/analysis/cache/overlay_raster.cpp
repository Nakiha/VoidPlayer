#include "analysis/cache/overlay_raster.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace vr::analysis {
namespace {

bool surface_byte_size(int width,
                       int height,
                       size_t bytes_per_pixel,
                       size_t& out) {
    out = 0;
    if (width <= 0 || height <= 0 || bytes_per_pixel == 0) {
        return false;
    }
    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    if (w > std::numeric_limits<size_t>::max() / h) {
        return false;
    }
    const size_t pixels = w * h;
    if (pixels > std::numeric_limits<size_t>::max() / bytes_per_pixel) {
        return false;
    }
    out = pixels * bytes_per_pixel;
    return true;
}

bool surface_has_bytes(const std::vector<uint8_t>& pixels,
                       int width,
                       int height,
                       size_t bytes_per_pixel) {
    size_t expected = 0;
    return surface_byte_size(width, height, bytes_per_pixel, expected) &&
           pixels.size() >= expected;
}

void fill_bgra_span(uint8_t* dst, int pixel_count, OverlayColor color) {
    if (pixel_count <= 0) {
        return;
    }

    const uint8_t packed[4] = {color.b, color.g, color.r, color.a};
    for (int i = 0; i < pixel_count; ++i) {
        std::memcpy(dst + static_cast<size_t>(i) * 4, packed, sizeof(packed));
    }
}

} // namespace

void blend_overlay_pixel(std::vector<uint8_t>& pixels,
                         int width,
                         int height,
                         int x,
                         int y,
                         OverlayColor color) {
    if (!surface_has_bytes(pixels, width, height, 4)) {
        return;
    }
    if (x < 0 || y < 0 || x >= width || y >= height || color.a == 0) {
        return;
    }
    const size_t off = static_cast<size_t>(y * width + x) * 4;
    const uint32_t src_a = color.a;
    const uint32_t inv_a = 255 - src_a;
    pixels[off + 0] = static_cast<uint8_t>((color.b * src_a + pixels[off + 0] * inv_a) / 255);
    pixels[off + 1] = static_cast<uint8_t>((color.g * src_a + pixels[off + 1] * inv_a) / 255);
    pixels[off + 2] = static_cast<uint8_t>((color.r * src_a + pixels[off + 2] * inv_a) / 255);
    pixels[off + 3] = static_cast<uint8_t>(std::min<uint32_t>(255, src_a + pixels[off + 3]));
}

void fill_overlay_rect(std::vector<uint8_t>& pixels,
                       int width,
                       int height,
                       int x0,
                       int y0,
                       int x1,
                       int y1,
                       OverlayColor color,
                       OverlayRasterStats* stats) {
    if (!surface_has_bytes(pixels, width, height, 4)) {
        return;
    }
    x0 = std::clamp(x0, 0, width);
    x1 = std::clamp(x1, 0, width);
    y0 = std::clamp(y0, 0, height);
    y1 = std::clamp(y1, 0, height);
    if (x0 >= x1 || y0 >= y1) return;
    if (stats) {
        stats->filled_pixels +=
            static_cast<uint64_t>(x1 - x0) * static_cast<uint64_t>(y1 - y0);
    }
    const int rect_width = x1 - x0;
    for (int y = y0; y < y1; ++y) {
        size_t off = static_cast<size_t>(y * width + x0) * 4;
        fill_bgra_span(pixels.data() + off, rect_width, color);
    }
}

void set_overlay_mask_pixel(std::vector<uint8_t>& pixels,
                            int width,
                            int height,
                            int x,
                            int y) {
    if (!surface_has_bytes(pixels, width, height, 4)) {
        return;
    }
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    const size_t off = static_cast<size_t>(y * width + x) * 4;
    pixels[off + 0] = 255;
    pixels[off + 1] = 255;
    pixels[off + 2] = 255;
    pixels[off + 3] = 255;
}

void stroke_overlay_rect_mask(std::vector<uint8_t>& pixels,
                              int width,
                              int height,
                              int x0,
                              int y0,
                              int x1,
                              int y1) {
    if (!surface_has_bytes(pixels, width, height, 4)) {
        return;
    }
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    x0 = std::clamp(x0, 0, width - 1);
    x1 = std::clamp(x1, 0, width - 1);
    y0 = std::clamp(y0, 0, height - 1);
    y1 = std::clamp(y1, 0, height - 1);
    if (x0 >= x1 || y0 >= y1) return;
    for (int x = x0; x <= x1; ++x) {
        set_overlay_mask_pixel(pixels, width, height, x, y0);
        set_overlay_mask_pixel(pixels, width, height, x, y1);
    }
    for (int y = y0; y <= y1; ++y) {
        set_overlay_mask_pixel(pixels, width, height, x0, y);
        set_overlay_mask_pixel(pixels, width, height, x1, y);
    }
}

void set_overlay_mask_pixel8(std::vector<uint8_t>& pixels,
                             int width,
                             int height,
                             int x,
                             int y) {
    if (!surface_has_bytes(pixels, width, height, 1)) {
        return;
    }
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    pixels[static_cast<size_t>(y * width + x)] = 255;
}

void stroke_overlay_rect_mask8(std::vector<uint8_t>& pixels,
                               int width,
                               int height,
                               int x0,
                               int y0,
                               int x1,
                               int y1) {
    if (!surface_has_bytes(pixels, width, height, 1)) {
        return;
    }
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    x0 = std::clamp(x0, 0, width - 1);
    x1 = std::clamp(x1, 0, width - 1);
    y0 = std::clamp(y0, 0, height - 1);
    y1 = std::clamp(y1, 0, height - 1);
    if (x0 >= x1 || y0 >= y1) return;

    const size_t top_off = static_cast<size_t>(y0) * static_cast<size_t>(width) +
                           static_cast<size_t>(x0);
    const size_t bottom_off = static_cast<size_t>(y1) * static_cast<size_t>(width) +
                              static_cast<size_t>(x0);
    const size_t span = static_cast<size_t>(x1 - x0 + 1);
    std::memset(pixels.data() + top_off, 255, span);
    if (y1 != y0) {
        std::memset(pixels.data() + bottom_off, 255, span);
    }

    for (int y = y0; y <= y1; ++y) {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(width);
        pixels[row + static_cast<size_t>(x0)] = 255;
        pixels[row + static_cast<size_t>(x1)] = 255;
    }
}

void draw_overlay_line(std::vector<uint8_t>& pixels,
                       int width,
                       int height,
                       int x0,
                       int y0,
                       int x1,
                       int y1,
                       OverlayColor color) {
    if (!surface_has_bytes(pixels, width, height, 4)) {
        return;
    }
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        blend_overlay_pixel(pixels, width, height, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

bool overlay_frame_covers_surface(const VachunkOverlayFrameData& frame,
                                  uint32_t video_width,
                                  uint32_t video_height,
                                  int surface_width,
                                  int surface_height) {
    if (video_width == 0 || video_height == 0 || surface_width <= 0 || surface_height <= 0) {
        return false;
    }
    const float scale_x = static_cast<float>(surface_width) / static_cast<float>(video_width);
    const float scale_y = static_cast<float>(surface_height) / static_cast<float>(video_height);
    uint64_t covered_pixels = 0;
    for (const auto& cu : frame.cus) {
        const auto& c = cu.common;
        int x0 = static_cast<int>(std::floor(static_cast<float>(c.x) * scale_x));
        int y0 = static_cast<int>(std::floor(static_cast<float>(c.y) * scale_y));
        int x1 = static_cast<int>(std::ceil(static_cast<float>(c.x + c.w) * scale_x));
        int y1 = static_cast<int>(std::ceil(static_cast<float>(c.y + c.h) * scale_y));
        x0 = std::clamp(x0, 0, surface_width);
        x1 = std::clamp(x1, 0, surface_width);
        y0 = std::clamp(y0, 0, surface_height);
        y1 = std::clamp(y1, 0, surface_height);
        if (x1 <= x0 || y1 <= y0) continue;
        covered_pixels +=
            static_cast<uint64_t>(x1 - x0) * static_cast<uint64_t>(y1 - y0);
        const uint64_t surface_pixels =
            static_cast<uint64_t>(surface_width) * static_cast<uint64_t>(surface_height);
        if (covered_pixels > surface_pixels) {
            return false;
        }
    }
    return covered_pixels ==
        static_cast<uint64_t>(surface_width) * static_cast<uint64_t>(surface_height);
}

OverlayColor heatmap_ramp_color(float value, uint8_t alpha) {
    const float t = std::clamp(value, 0.0f, 1.0f);
    const float r = t < 0.5f ? (t * 2.0f) : 1.0f;
    const float g = t < 0.5f ? 1.0f : (1.0f - (t - 0.5f) * 2.0f);
    const float brightness = 0.55f + 0.45f * t;
    const uint8_t red = static_cast<uint8_t>(std::round(255.0f * r * brightness));
    const uint8_t green = static_cast<uint8_t>(std::round(255.0f * g * brightness));
    const uint8_t blue = static_cast<uint8_t>(std::round(36.0f * (1.0f - t)));
    return OverlayColor{blue, green, red, alpha};
}

OverlayColor qp_color(uint8_t qp, uint8_t alpha) {
    static const auto lut = [] {
        std::array<OverlayColor, 51> colors = {};
        for (size_t i = 0; i < colors.size(); ++i) {
            colors[i] = heatmap_ramp_color(static_cast<float>(i) / 50.0f, 255);
        }
        return colors;
    }();
    OverlayColor color = lut[std::min<uint8_t>(qp, 50)];
    color.a = alpha;
    return color;
}

OverlayColor cu_bit_density_color(const VachunkCuCommon& cu, uint8_t alpha) {
    static const auto lut = [] {
        std::array<OverlayColor, 4097> colors = {};
        const float low = std::log2(64.0f + 1.0f);
        const float high = std::log2(4096.0f + 1.0f);
        for (size_t i = 0; i < colors.size(); ++i) {
            const float t = std::clamp(
                (std::log2(static_cast<float>(i) + 1.0f) - low) / (high - low),
                0.0f,
                1.0f);
            colors[i] = heatmap_ramp_color(t, 255);
        }
        return colors;
    }();

    const uint64_t area =
        std::max<uint64_t>(1, static_cast<uint64_t>(cu.w) * static_cast<uint64_t>(cu.h));
    const uint64_t density =
        (static_cast<uint64_t>(cu.bit_count) * 4096ull + area / 2ull) / area;
    OverlayColor color = lut[static_cast<size_t>(std::min<uint64_t>(density, 4096ull))];
    color.a = alpha;
    return color;
}

OverlayColor pred_color(uint8_t pred_mode, const VachunkCuInter& inter, uint8_t alpha) {
    if (pred_mode == 1) return OverlayColor{80, 235, 90, alpha};
    if (inter.skip != 0) return OverlayColor{40, 220, 245, alpha};
    if (inter.merge_flag != 0) return OverlayColor{235, 170, 80, alpha};
    return OverlayColor{245, 120, 70, alpha};
}

bool raster_overlay_heatmap(const VachunkOverlayFrameData& frame,
                            uint32_t video_width,
                            uint32_t video_height,
                            int surface_width,
                            int surface_height,
                            OverlayHeatmapMode mode,
                            uint8_t alpha,
                            std::vector<uint8_t>& pixels,
                            OverlayRasterStats* stats) {
    size_t expected = 0;
    if (video_width == 0 ||
        video_height == 0 ||
        !surface_byte_size(surface_width, surface_height, 4, expected)) {
        return false;
    }
    if (expected > pixels.max_size()) {
        return false;
    }
    if (pixels.size() != expected) {
        pixels.resize(expected);
    }
    if (!overlay_frame_covers_surface(
            frame, video_width, video_height, surface_width, surface_height)) {
        std::fill(pixels.begin(), pixels.end(), 0);
    }
    if (stats) {
        *stats = {};
        stats->cu_count = frame.cus.size();
    }

    const float scale_x = static_cast<float>(surface_width) / static_cast<float>(video_width);
    const float scale_y = static_cast<float>(surface_height) / static_cast<float>(video_height);
    for (const auto& cu : frame.cus) {
        const auto& c = cu.common;
        const int x0 = static_cast<int>(std::floor(static_cast<float>(c.x) * scale_x));
        const int y0 = static_cast<int>(std::floor(static_cast<float>(c.y) * scale_y));
        const int x1 = static_cast<int>(std::ceil(static_cast<float>(c.x + c.w) * scale_x));
        const int y1 = static_cast<int>(std::ceil(static_cast<float>(c.y + c.h) * scale_y));
        const OverlayColor color = mode == OverlayHeatmapMode::BitCost
            ? cu_bit_density_color(c, alpha)
            : qp_color(c.qp, alpha);
        fill_overlay_rect(pixels, surface_width, surface_height, x0, y0, x1, y1, color, stats);
    }
    return true;
}

} // namespace vr::analysis
