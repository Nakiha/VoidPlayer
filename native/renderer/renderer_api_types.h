#pragma once

#include "audio/audio_output_stats.h"
#include "renderer/render/presentation_backend_types.h"
#include "renderer/time/media_timestamp_constants.h"
#include "renderer/track/track_gpu_memory_stats_types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace vr {

struct RendererEvent {
    enum class Type {
        SeekPreviewPresented,
        TrackError,
        PlaybackClock,
        PlaybackFrameReady,
    };

    Type type = Type::SeekPreviewPresented;
    int64_t request_id = -1;
    int track_file_id = -1;
    int64_t pts_us = -1;
    int64_t dts_us = kNoTimestampUs;
    int64_t target_pts_us = -1;
    int64_t duration_us = 0;
    double playback_speed = 1.0;
    bool playing = false;
    int error_code = 0;
};

using RendererEventCallback = std::function<void(const RendererEvent&)>;
using RendererFrameCallback =
    std::function<void(const PresentationBackendFrameInfo*)>;

enum class RendererFrameRefreshResult : uint8_t {
    Presented,
    NotReady,
    Failed,
};

struct RendererGpuMemoryStats {
    uint64_t total_estimated_bytes = 0;
    uint64_t decoder_pool_bytes = 0;
    uint64_t exact_seek_snapshot_bytes = 0;
    uint64_t presenter_texture_bytes = 0;
    uint64_t fp16_target_bytes = 0;
    uint64_t analysis_overlay_bytes = 0;
    uint64_t cpu_frame_bytes = 0;
    uint64_t track_buffer_cpu_bytes = 0;
    uint64_t packet_queue_bytes = 0;
    uint64_t exact_seek_candidate_cpu_bytes = 0;
    uint64_t exact_seek_stable_cpu_bytes = 0;
    size_t exact_seek_budget_drop_count = 0;
    int analysis_overlay_width = 0;
    int analysis_overlay_height = 0;
    std::vector<TrackGpuMemoryStats> tracks;
};

} // namespace vr
