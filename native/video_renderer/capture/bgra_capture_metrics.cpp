#include "video_renderer/capture/bgra_capture_metrics.h"

#include <algorithm>

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

} // namespace vr
