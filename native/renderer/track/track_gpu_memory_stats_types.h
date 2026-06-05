#pragma once

#include <cstddef>
#include <cstdint>

namespace vr {

struct TrackGpuMemoryStats {
    int slot = -1;
    int file_id = 0;
    bool hardware_enabled = false;
    bool hardware_download_to_cpu = false;
    int hw_format = 0;
    int sw_format = 0;
    int hw_width = 0;
    int hw_height = 0;
    int hw_initial_pool_size = 0;
    int extra_hw_frames = 0;
    uint64_t decoder_frame_bytes = 0;
    uint64_t decoder_pool_bytes = 0;
    uint64_t exact_seek_snapshot_bytes = 0;
    uint64_t presenter_copy_texture_bytes = 0;
    uint64_t track_buffer_cpu_bytes = 0;
    uint64_t packet_queue_bytes = 0;
    uint64_t exact_seek_candidate_cpu_bytes = 0;
    uint64_t exact_seek_stable_cpu_bytes = 0;
    uint64_t total_cpu_frame_bytes = 0;
    size_t exact_seek_reorder_count = 0;
    size_t exact_seek_pending_count = 0;
    size_t exact_seek_stable_frame_count = 0;
    size_t exact_seek_budget_drop_count = 0;
    size_t buffer_count = 0;
    size_t buffer_capacity = 0;
};

} // namespace vr
