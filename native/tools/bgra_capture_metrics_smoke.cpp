#include "video_renderer/capture/bgra_capture_metrics.h"

#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

bool near(double lhs, double rhs) {
    return std::abs(lhs - rhs) < 0.000001;
}

} // namespace

int main() {
    const uint8_t bgra[] = {
        10, 20, 30, 255,
        0, 0, 0, 255,
        99, 99, 99, 255,
        99, 99, 99, 255,
    };

    const auto metrics = vr::measure_bgra_capture(bgra, 2, 2, 8);
    const double expected_luma =
        (0.2126 * 30.0 + 0.7152 * 20.0 + 0.0722 * 10.0 + 99.0 + 99.0) / 4.0;
    if (metrics.width != 2 || metrics.height != 2 ||
        !near(metrics.avg_luma, expected_luma) ||
        !near(metrics.non_black_ratio, 0.75)) {
        std::cerr << "unexpected metrics: "
                  << metrics.width << "x" << metrics.height
                  << " avg_luma=" << metrics.avg_luma
                  << " non_black=" << metrics.non_black_ratio << "\n";
        return 1;
    }

    const auto changed = vr::measure_bgra_capture(bgra + 4, 1, 1, 4);
    if (changed.hash == metrics.hash) {
        std::cerr << "hash did not change for a different BGRA region\n";
        return 1;
    }

    const auto invalid = vr::measure_bgra_capture(nullptr, 2, 2, 8);
    if (invalid.avg_luma != 0.0 || invalid.non_black_ratio != 0.0) {
        std::cerr << "invalid capture should produce zero-valued metrics\n";
        return 1;
    }

    return 0;
}
