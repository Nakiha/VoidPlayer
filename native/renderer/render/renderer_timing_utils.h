#pragma once

#include <chrono>
#include <cstdint>

namespace vr {

// Max render-loop sleep for playback deadline responsiveness. Viewport layout
// redraw cadence is driven by the platform display clock on macOS.
static constexpr int64_t MAX_SLEEP_US = 8000;
static constexpr auto kTransientPresentationBackpressureBackoff =
    std::chrono::microseconds(MAX_SLEEP_US);

inline uint64_t elapsed_us_since(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}

inline int64_t steady_clock_us_now() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace vr
