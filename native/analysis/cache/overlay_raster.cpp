#include "analysis/cache/overlay_raster.h"

#include <algorithm>
#include <cmath>

namespace vr::analysis {

void blend_overlay_pixel(std::vector<uint8_t>& pixels,
                         int width,
                         int height,
                         int x,
                         int y,
                         OverlayColor color) {
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
    x0 = std::clamp(x0, 0, width);
    x1 = std::clamp(x1, 0, width);
    y0 = std::clamp(y0, 0, height);
    y1 = std::clamp(y1, 0, height);
    if (x0 >= x1 || y0 >= y1) return;
    if (stats) {
        stats->filled_pixels +=
            static_cast<uint64_t>(x1 - x0) * static_cast<uint64_t>(y1 - y0);
    }
    for (int y = y0; y < y1; ++y) {
        size_t off = static_cast<size_t>(y * width + x0) * 4;
        for (int x = x0; x < x1; ++x, off += 4) {
            pixels[off + 0] = color.b;
            pixels[off + 1] = color.g;
            pixels[off + 2] = color.r;
            pixels[off + 3] = color.a;
        }
    }
}

void set_overlay_mask_pixel(std::vector<uint8_t>& pixels,
                            int width,
                            int height,
                            int x,
                            int y) {
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
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    x0 = std::clamp(x0, 0, width - 1);
    x1 = std::clamp(x1, 0, width - 1);
    y0 = std::clamp(y0, 0, height - 1);
    y1 = std::clamp(y1, 0, height - 1);
    if (x0 >= x1 || y0 >= y1) return;
    for (int x = x0; x <= x1; ++x) {
        set_overlay_mask_pixel8(pixels, width, height, x, y0);
        set_overlay_mask_pixel8(pixels, width, height, x, y1);
    }
    for (int y = y0; y <= y1; ++y) {
        set_overlay_mask_pixel8(pixels, width, height, x0, y);
        set_overlay_mask_pixel8(pixels, width, height, x1, y);
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
    const float t = std::clamp(static_cast<float>(qp) / 50.0f, 0.0f, 1.0f);
    return heatmap_ramp_color(t, alpha);
}

OverlayColor cu_bit_density_color(const VachunkCuCommon& cu, uint8_t alpha) {
    const float area = std::max(1.0f, static_cast<float>(cu.w) * static_cast<float>(cu.h));
    const float bits_per_64x64 =
        static_cast<float>(cu.bit_count) * (64.0f * 64.0f) / area;
    const float low = std::log2(64.0f + 1.0f);
    const float high = std::log2(4096.0f + 1.0f);
    const float t = std::clamp(
        (std::log2(bits_per_64x64 + 1.0f) - low) / (high - low),
        0.0f,
        1.0f);
    return heatmap_ramp_color(t, alpha);
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
    if (video_width == 0 || video_height == 0 || surface_width <= 0 || surface_height <= 0) {
        return false;
    }
    const size_t expected =
        static_cast<size_t>(surface_width) * static_cast<size_t>(surface_height) * 4;
    if (pixels.size() != expected) {
        pixels.resize(expected);
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
