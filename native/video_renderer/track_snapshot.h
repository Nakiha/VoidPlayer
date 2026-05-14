#pragma once

#include "video_renderer/decode/decode_thread.h"
#include "video_renderer/track_gpu_memory_stats.h"
#include "video_renderer/track_info.h"
#include "video_renderer/track_perf_stats.h"
#include "video_renderer/track_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vr {

struct TrackPerfSnapshotResult {
    TrackPerfStats stats;
    uint64_t frames_decoded = 0;
};

std::vector<TrackInfo> snapshot_track_infos(const TrackPipelineManager& tracks);

TrackPerfSnapshotResult snapshot_track_perf_stats(
    size_t slot,
    const TrackPipeline& track,
    const DecodePerfCounters::Snapshot& decode_perf,
    const std::optional<TextureFrame>& current_frame,
    uint64_t baseline_frames,
    double elapsed_s);

TrackGpuMemoryStats snapshot_track_gpu_memory_stats(
    size_t slot,
    const TrackPipeline& track,
    const DecodeMemoryStats* decode_stats,
    uint64_t presenter_copy_texture_bytes);

} // namespace vr
