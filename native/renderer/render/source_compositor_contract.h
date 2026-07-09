#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace vr {

inline constexpr int kSourceCompositorMaxTracks = 4;
inline constexpr int kSourceCompositorLiveBufferCount = 3;
inline constexpr uint64_t kSourceCompositorDefaultBudgetBytes =
    384ull * 1024ull * 1024ull;

struct SourceCompositorTrackDescriptor {
    int slot = -1;
    int file_id = -1;
    int width = 0;
    int height = 0;
    int color_transfer = 0;
};

struct SourceCompositorProjection {
    bool enabled = false;
    int mode = 0;
    float split_pos = 0.5f;
    int active_track_count = 1;
    std::array<int, kSourceCompositorMaxTracks> source_order = {0, 1, 2, 3};
    std::array<float, kSourceCompositorMaxTracks> display_offset_x{};
    std::array<float, kSourceCompositorMaxTracks> display_offset_y{};
    std::array<float, kSourceCompositorMaxTracks> inv_display_size_x{};
    std::array<float, kSourceCompositorMaxTracks> inv_display_size_y{};
    std::array<float, kSourceCompositorMaxTracks> view_offset_uv_x{};
    std::array<float, kSourceCompositorMaxTracks> view_offset_uv_y{};
};

struct SourceCompositorProjectionSample {
    bool present = false;
    int source_slot = -1;
    float u = 0.0f;
    float v = 0.0f;
};

struct SourceCompositorRetainedVisualRect {
    bool present = false;
    int source_slot = -1;
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    float clip_left = 0.0f;
    float clip_top = 0.0f;
    float clip_right = 0.0f;
    float clip_bottom = 0.0f;
};

struct SourceCompositorRingPolicy {
    int depth = 0;
    uint64_t bytes_per_frame = 0;
    uint64_t total_bytes = 0;
    bool frozen_snapshot = false;
    bool allowed = false;
};

SourceCompositorRingPolicy resolve_source_compositor_ring_policy(
    const std::vector<SourceCompositorTrackDescriptor>& descriptors,
    uint64_t budget_bytes,
    uint64_t bytes_per_pixel = 8);

SourceCompositorProjectionSample project_source_compositor_sample(
    float video_u,
    float video_v,
    const SourceCompositorProjection& projection,
    const std::array<bool, kSourceCompositorMaxTracks>& source_present);

std::array<SourceCompositorRetainedVisualRect, kSourceCompositorMaxTracks>
project_source_compositor_retained_visuals(
    float viewport_left,
    float viewport_top,
    float viewport_right,
    float viewport_bottom,
    const SourceCompositorProjection& projection,
    const std::array<bool, kSourceCompositorMaxTracks>& source_present);

} // namespace vr
