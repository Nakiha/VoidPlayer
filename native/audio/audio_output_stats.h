#pragma once

#include <cstddef>
#include <cstdint>

namespace vr {

struct AudioOutputStats {
    bool device_initialized = false;
    bool playing = false;
    int active_track = -1;
    int output_sample_rate = 0;
    int output_channels = 0;
    size_t registered_track_count = 0;
    bool active_track_registered = false;
    size_t active_track_queued_frames = 0;
    int64_t active_track_queued_duration_us = 0;
    size_t active_track_underrun_frames = 0;
    size_t active_track_discarded_frames = 0;
    size_t active_track_seek_trimmed_frames = 0;
};

} // namespace vr
