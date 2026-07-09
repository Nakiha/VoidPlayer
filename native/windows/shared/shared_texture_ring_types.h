#pragma once

#include "renderer/render/source_compositor_contract.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace vr {

struct AnalysisOverlayPrimitivePackage;

inline constexpr int kSharedFp16TextureRingBufferCount = 3;
inline constexpr int kSharedSourceCacheLiveBufferCount =
    kSourceCompositorLiveBufferCount;
inline constexpr uint64_t kSharedSourceCacheDefaultBudgetBytes =
    kSourceCompositorDefaultBudgetBytes;

enum class SharedFp16TextureSyncMode : uint32_t {
    KeyedMutex = 0,
    PublishedAfterProducerWait = 1,
};

struct SharedFp16TextureSnapshot {
    HANDLE handle = nullptr;
    int width = 0;
    int height = 0;
    int buffer_index = -1;
    uint64_t ring_generation = 0;
    uint64_t frame_generation = 0;
    uint64_t external_flutter_frame_generation = 0;
    SharedFp16TextureSyncMode sync_mode = SharedFp16TextureSyncMode::KeyedMutex;
    uint64_t consumer_acquire_key = 1;
    uint64_t producer_release_key = 0;
};

struct SharedFp16RingPrewarmStats {
    uint64_t request_count = 0;
    uint64_t ready_count = 0;
    uint64_t hit_count = 0;
    uint64_t dropped_count = 0;
    uint64_t consumed_count = 0;
};

using SourceCacheTrackDescriptor = SourceCompositorTrackDescriptor;

enum class SharedSourceCacheTextureSyncMode : uint32_t {
    KeyedMutex = 0,
    PublishedAfterProducerWait = 1,
};

struct SharedSourceCacheTextureSnapshot {
    HANDLE handle = nullptr;
    int source_slot = -1;
    int source_file_id = -1;
    int width = 0;
    int height = 0;
    int color_transfer = 0;
    SharedSourceCacheTextureSyncMode sync_mode =
        SharedSourceCacheTextureSyncMode::KeyedMutex;
    uint64_t consumer_acquire_key = 1;
    uint64_t producer_release_key = 0;
};

struct SharedSourceCacheBundleSnapshot {
    std::array<SharedSourceCacheTextureSnapshot, 4> textures{};
    size_t texture_count = 0;
    int buffer_index = -1;
    int ring_depth = 0;
    uint64_t ring_generation = 0;
    uint64_t frame_generation = 0;
    std::shared_ptr<const AnalysisOverlayPrimitivePackage> overlay;
};

using SourceCacheRingPolicy = SourceCompositorRingPolicy;

struct SourceCachePublishInfo {
    uint64_t ring_generation = 0;
    uint64_t frame_generation = 0;
    size_t texture_count = 0;
};

SourceCacheRingPolicy resolve_source_cache_ring_policy(
    const std::vector<SourceCacheTrackDescriptor>& descriptors,
    uint64_t budget_bytes);

} // namespace vr
