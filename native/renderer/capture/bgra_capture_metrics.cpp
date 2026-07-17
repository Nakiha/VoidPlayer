#include "renderer/capture/bgra_capture_metrics.h"

#include <algorithm>
#include <array>

namespace vr {

namespace {

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void hash_byte(uint64_t& hash, uint8_t value) {
    hash ^= static_cast<uint64_t>(value);
    hash *= kFnvPrime;
}

} // namespace

BgraCaptureMetrics measure_bgra_capture(const uint8_t* bgra,
                                        int width,
                                        int height,
                                        int stride_bytes) noexcept {
    BgraCaptureMetrics metrics;
    metrics.width = std::max(0, width);
    metrics.height = std::max(0, height);
    metrics.hash = kFnvOffsetBasis;

    if (!bgra || width <= 0 || height <= 0 || stride_bytes < width * 4) {
        return metrics;
    }

    double luma_sum = 0.0;
    int non_black = 0;
    uint64_t hash = kFnvOffsetBasis;

    for (int y = 0; y < height; ++y) {
        const uint8_t* row = bgra + static_cast<size_t>(y) * stride_bytes;
        for (int x = 0; x < width; ++x) {
            const uint8_t b = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t r = row[x * 4 + 2];
            luma_sum += 0.2126 * static_cast<double>(r) +
                        0.7152 * static_cast<double>(g) +
                        0.0722 * static_cast<double>(b);
            if (r > 4 || g > 4 || b > 4) {
                ++non_black;
            }

            hash_byte(hash, r);
            hash_byte(hash, g);
            hash_byte(hash, b);
        }
    }

    const int pixel_count = width * height;
    metrics.avg_luma = luma_sum / static_cast<double>(pixel_count);
    metrics.non_black_ratio =
        static_cast<double>(non_black) / static_cast<double>(pixel_count);
    metrics.hash = hash;
    return metrics;
}

BgraOverlayLineStyleMetrics measure_bgra_overlay_line_style(
    const uint8_t* bgra,
    int width,
    int height,
    int stride_bytes) noexcept {
    BgraOverlayLineStyleMetrics metrics;
    if (!bgra || width < 6 || height < 6 || stride_bytes < width * 4) {
        return metrics;
    }

    const auto channels = [bgra, stride_bytes](int x, int y) {
        const uint8_t* pixel =
            bgra + static_cast<size_t>(y) * stride_bytes + x * 4;
        return std::array<int, 3>{pixel[2], pixel[1], pixel[0]};
    };
    const auto luma = [&channels](int x, int y) {
        const auto rgb = channels(x, y);
        return (77 * rgb[0] + 150 * rgb[1] + 29 * rgb[2]) >> 8;
    };
    const auto chroma = [&channels](int x, int y) {
        const auto rgb = channels(x, y);
        return *std::max_element(rgb.begin(), rgb.end()) -
               *std::min_element(rgb.begin(), rgb.end());
    };
    const auto white_at = [&luma, &chroma](int x, int y) {
        return luma(x, y) >= 190 && chroma(x, y) <= 80;
    };
    const auto black_at = [&luma, &chroma](int x, int y) {
        return luma(x, y) <= 88 && chroma(x, y) <= 70;
    };
    const auto near_white = [&white_at](int x, int y) {
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                if ((dx != 0 || dy != 0) && white_at(x + dx, y + dy)) {
                    return true;
                }
            }
        }
        return false;
    };

    for (int y = 2; y < height - 2; ++y) {
        for (int x = 2; x < width - 2; ++x) {
            if (white_at(x, y)) {
                const bool horizontal_halo =
                    black_at(x - 1, y) && black_at(x + 1, y);
                const bool vertical_halo =
                    black_at(x, y - 1) && black_at(x, y + 1);
                if (horizontal_halo || vertical_halo) {
                    ++metrics.paired_centers;
                    continue;
                }
                const bool horizontal_run =
                    white_at(x - 1, y) || white_at(x + 1, y);
                const bool vertical_run =
                    white_at(x, y - 1) || white_at(x, y + 1);
                if (horizontal_run || vertical_run) {
                    ++metrics.weak_white_centers;
                }
                continue;
            }

            if (!black_at(x, y) || near_white(x, y)) {
                continue;
            }
            const bool horizontal_run =
                black_at(x - 1, y) || black_at(x + 1, y);
            const bool vertical_run =
                black_at(x, y - 1) || black_at(x, y + 1);
            if (horizontal_run || vertical_run) {
                ++metrics.black_only_centers;
            }
        }
    }
    return metrics;
}

} // namespace vr
