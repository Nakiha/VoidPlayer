#pragma once

#include <cstdint>

namespace vr {

struct BgraCaptureMetrics {
    int width = 0;
    int height = 0;
    double avg_luma = 0.0;
    double non_black_ratio = 0.0;
    uint64_t hash = 14695981039346656037ull;
};

BgraCaptureMetrics measure_bgra_capture(const uint8_t* bgra,
                                        int width,
                                        int height,
                                        int stride_bytes) noexcept;

} // namespace vr
