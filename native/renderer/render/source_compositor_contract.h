#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vr {

inline constexpr int kSourceCompositorMaxTracks = 4;
inline constexpr int kSourceCompositorLiveBufferCount = 3;
inline constexpr uint64_t kSourceCompositorDefaultBudgetBytes =
    384ull * 1024ull * 1024ull;

// Platform resources never cross this contract. Metal textures, IOSurfaces,
// D3D resources, and synchronization primitives belong to the platform lease
// that carries this metadata.
enum class SourceCompositorPixelFormat : uint8_t {
    Unknown,
    Bgra8Unorm,
    Rgba16Float,
};

enum class SourceCompositorColorEncoding : uint8_t {
    Unknown,
    CompositorReadySDR,
    LinearExtended,
};

enum class SourceCompositorAlphaMode : uint8_t {
    Unknown,
    Opaque,
    Premultiplied,
};

struct SourceCompositorOutputContract {
    SourceCompositorPixelFormat pixel_format =
        SourceCompositorPixelFormat::Unknown;
    SourceCompositorColorEncoding color_encoding =
        SourceCompositorColorEncoding::Unknown;
    SourceCompositorAlphaMode alpha_mode =
        SourceCompositorAlphaMode::Unknown;
    uint32_t bytes_per_pixel = 0;
    bool extended_range = false;
};

enum class SourceCompositorLifecycleState : uint8_t {
    Unconfigured,
    Allocating,
    Ready,
    Publishing,
    Draining,
};

enum class SourceCompositorLifecycleEventType : uint8_t {
    BeginAllocation,
    MarkReady,
    Publish,
    BeginDraining,
    Reset,
};

struct SourceCompositorLifecycleEvent {
    SourceCompositorLifecycleEventType type =
        SourceCompositorLifecycleEventType::Reset;
    uint64_t topology_generation = 0;
    uint64_t ring_generation = 0;
    uint64_t frame_generation = 0;
};

struct SourceCompositorLifecycle {
    SourceCompositorLifecycleState state =
        SourceCompositorLifecycleState::Unconfigured;
    uint64_t topology_generation = 0;
    uint64_t ring_generation = 0;
    uint64_t frame_generation = 0;
    uint64_t publish_count = 0;
};

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

struct SourceCompositorPackageMetadata {
    SourceCompositorOutputContract output;
    uint64_t topology_generation = 0;
    uint64_t ring_generation = 0;
    uint64_t frame_generation = 0;
    int buffer_index = -1;
    int ring_depth = 0;
    size_t track_count = 0;
    uint64_t required_slot_mask = 0;
    uint64_t published_slot_mask = 0;
    bool frozen_snapshot = false;
};

SourceCompositorOutputContract source_compositor_sdr_output_contract();
SourceCompositorOutputContract source_compositor_edr_output_contract();

bool validate_source_compositor_output_contract(
    const SourceCompositorOutputContract& output);

bool validate_source_compositor_descriptors(
    const std::vector<SourceCompositorTrackDescriptor>& descriptors);

bool validate_source_compositor_package(
    const SourceCompositorPackageMetadata& package,
    const std::vector<SourceCompositorTrackDescriptor>& descriptors);

bool apply_source_compositor_lifecycle_event(
    SourceCompositorLifecycle& lifecycle,
    const SourceCompositorLifecycleEvent& event);

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
