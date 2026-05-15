#pragma once

#include <cstdint>
#include <string>

namespace vr {

/// Track metadata returned to the UI layer.
struct TrackInfo {
    int file_id = 0;       ///< Stable identifier (auto-incrementing, survives reorder)
    int slot = -1;
    std::string file_path;
    int width = 0;
    int height = 0;
    int64_t duration_us = 0;
    int64_t start_time_us = 0;
    int64_t bit_rate = 0;
    std::string format_name;
    std::string codec_name;
    std::string codec_long_name;
    std::string decoder_name;
};

} // namespace vr
