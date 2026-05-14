#pragma once

#include <cstdint>

/// ---- dart:ffi flat struct for diagnostics (no heap, no string) ----

constexpr int kMaxTracksFFI = 4;

struct NakiVrTrackStats {
    int32_t  slot;            // -1 = unused
    int32_t  file_id;
    double   fps;
    double   avg_decode_ms;
    double   max_decode_ms;
    int32_t  buffer_count;
    int32_t  buffer_capacity;
    int32_t  buffer_state;    // TrackState enum value
    uint64_t cpu_frame_memory_bytes;
    uint64_t packet_queue_memory_bytes;
    int64_t  current_pts_us;
    int64_t  current_dts_us;
};

struct NakiVrDiagnostics {
    double   playback_time_s;
    int32_t  is_playing;
    int32_t  track_count;
    uint64_t process_working_set_bytes;
    uint64_t process_private_bytes;
    uint64_t dedicated_video_memory_bytes;
    uint64_t cpu_frame_memory_bytes;
    uint64_t packet_queue_memory_bytes;
    NakiVrTrackStats tracks[kMaxTracksFFI];
    int32_t d3d_device_lost;
    int32_t reserved0;
    int64_t d3d_device_removed_reason;
};
