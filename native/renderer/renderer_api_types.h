#pragma once

#include "audio/audio_output_stats.h"
#include "renderer/buffer/bidi_ring_buffer.h"
#include "renderer/render/presentation_backend_types.h"
#include "renderer/track/track_gpu_memory_stats.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace vr {

struct RendererEvent {
    enum class Type {
        SeekPreviewPresented,
        TrackError,
    };

    Type type = Type::SeekPreviewPresented;
    int64_t request_id = -1;
    int track_file_id = -1;
    int64_t pts_us = -1;
    int64_t dts_us = kNoTimestampUs;
    int64_t target_pts_us = -1;
    int error_code = 0;
};

using RendererEventCallback = std::function<void(const RendererEvent&)>;
using RendererFrameCallback =
    std::function<void(const PresentationBackendFrameInfo*)>;

using D3D11BackendMetrics = PresentationBackendMetrics;

struct RendererGpuMemoryStats {
    uint64_t total_estimated_bytes = 0;
    uint64_t decoder_pool_bytes = 0;
    uint64_t exact_seek_snapshot_bytes = 0;
    uint64_t presenter_texture_bytes = 0;
    uint64_t headless_output_bytes = 0;
    uint64_t analysis_overlay_bytes = 0;
    uint64_t cpu_frame_bytes = 0;
    uint64_t track_buffer_cpu_bytes = 0;
    uint64_t packet_queue_bytes = 0;
    uint64_t exact_seek_candidate_cpu_bytes = 0;
    uint64_t exact_seek_stable_cpu_bytes = 0;
    size_t exact_seek_budget_drop_count = 0;
    int headless_width = 0;
    int headless_height = 0;
    int headless_buffer_count = 0;
    int analysis_overlay_width = 0;
    int analysis_overlay_height = 0;
    std::vector<TrackGpuMemoryStats> tracks;
};

enum class SharedTextureHandleType {
    None = 0,
    D3D11SharedHandle = 1,
};

struct SharedTextureSnapshot {
    SharedTextureHandleType type = SharedTextureHandleType::None;
    void* texture = nullptr;  ///< AddRef'd backend texture; caller must Release().
    void* handle = nullptr;
    int width = 0;
    int height = 0;
    int buffer_index = -1;
    uint64_t buffer_generation = 0;
};

} // namespace vr
