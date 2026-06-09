#pragma once

#include <chrono>
#include <cstdint>

namespace vr {

using MediaTime = std::chrono::microseconds;

constexpr MediaTime media_time_from_us(int64_t us) noexcept {
    return MediaTime{us};
}

constexpr int64_t media_time_us(MediaTime time) noexcept {
    return time.count();
}

inline double media_time_seconds(MediaTime time) noexcept {
    return static_cast<double>(media_time_us(time)) / 1e6;
}

} // namespace vr
