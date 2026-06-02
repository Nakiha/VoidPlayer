#pragma once

#include "renderer/decode/decode_thread.h"
#include "renderer/track/track_gpu_memory_stats.h"
#include "renderer/track/track_info.h"
#include "renderer/track/track_perf_baseline.h"
#include "renderer/track/track_perf_stats.h"
#include "renderer/track/track_pipeline.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vr {

struct TrackPerfSnapshotResult {
    TrackPerfStats stats;
    uint64_t frames_decoded = 0;
};

struct TrackPerfStatsCollectionResult {
    std::vector<TrackPerfStats> stats;
    std::array<std::optional<uint64_t>, kMaxTracks> frames_decoded_by_slot{};
};

struct TrackGpuMemoryStatsCollectionResult {
    std::vector<TrackGpuMemoryStats> tracks;
    uint64_t decoder_pool_bytes = 0;
    uint64_t exact_seek_snapshot_bytes = 0;
    uint64_t track_buffer_cpu_bytes = 0;
    uint64_t packet_queue_bytes = 0;
    uint64_t exact_seek_candidate_cpu_bytes = 0;
    uint64_t exact_seek_stable_cpu_bytes = 0;
    size_t exact_seek_budget_drop_count = 0;
    uint64_t cpu_frame_bytes = 0;
    uint64_t total_estimated_bytes = 0;
};

struct RenderLoopTrackDiagnosticSnapshot {
    size_t slot = 0;
    size_t buffer_count = 0;
    size_t buffer_capacity = 0;
    TrackState buffer_state = TrackState::Empty;
};

std::vector<TrackInfo> snapshot_track_infos(const TrackPipelineManager& tracks);

std::vector<RenderLoopTrackDiagnosticSnapshot>
snapshot_render_loop_track_diagnostics(const TrackPipelineManager& tracks);

TrackPerfSnapshotResult snapshot_track_perf_stats(
    size_t slot,
    const TrackPipeline& track,
    const DecodePerfCounters::Snapshot& decode_perf,
    const std::optional<TextureFrame>& current_frame,
    uint64_t baseline_frames,
    double elapsed_s);

TrackPerfStatsCollectionResult snapshot_track_perf_stats_collection(
    const TrackPipelineManager& tracks,
    const PresentDecision& last_decision,
    const TrackPerfBaselineTracker& baseline_tracker,
    double elapsed_s);

TrackGpuMemoryStats snapshot_track_gpu_memory_stats(
    size_t slot,
    const TrackPipeline& track,
    const DecodeMemoryStats* decode_stats,
    uint64_t presenter_copy_texture_bytes);

TrackGpuMemoryStatsCollectionResult snapshot_track_gpu_memory_stats_collection(
    const TrackPipelineManager& tracks,
    const std::array<uint64_t, kMaxTracks>& presenter_copy_texture_bytes_by_slot);

} // namespace vr
