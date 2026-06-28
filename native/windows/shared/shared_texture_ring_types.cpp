#include "windows/shared/shared_texture_ring_types.h"

#include <dxgi.h>
#include <limits>

namespace vr {
namespace {

bool valid_descriptor(const SourceCacheTrackDescriptor& descriptor) {
    return descriptor.slot >= 0 && descriptor.slot < 4 &&
           descriptor.file_id >= 0 &&
           descriptor.width > 0 && descriptor.height > 0;
}

uint64_t estimate_dxgi_surface_bytes(uint32_t width,
                                     uint32_t height,
                                     DXGI_FORMAT format) {
    if (width == 0 || height == 0) {
        return 0;
    }
    uint64_t bytes_per_pixel = 0;
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        bytes_per_pixel = 8;
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        bytes_per_pixel = 4;
        break;
    default:
        return 0;
    }
    const uint64_t pixels =
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (pixels > std::numeric_limits<uint64_t>::max() / bytes_per_pixel) {
        return 0;
    }
    return pixels * bytes_per_pixel;
}

} // namespace

SourceCacheRingPolicy resolve_source_cache_ring_policy(
    const std::vector<SourceCacheTrackDescriptor>& descriptors,
    uint64_t budget_bytes) {
    SourceCacheRingPolicy result;
    if (descriptors.empty() || descriptors.size() > 4 || budget_bytes == 0) {
        return result;
    }
    uint64_t bytes_per_frame = 0;
    std::array<bool, 4> slots{};
    for (const auto& descriptor : descriptors) {
        if (!valid_descriptor(descriptor) || slots[descriptor.slot]) {
            return result;
        }
        slots[descriptor.slot] = true;
        const uint64_t bytes = estimate_dxgi_surface_bytes(
            static_cast<uint32_t>(descriptor.width),
            static_cast<uint32_t>(descriptor.height),
            DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (bytes == 0 ||
            bytes_per_frame > std::numeric_limits<uint64_t>::max() - bytes) {
            return result;
        }
        bytes_per_frame += bytes;
    }
    result.bytes_per_frame = bytes_per_frame;
    result.depth =
        bytes_per_frame <=
                budget_bytes / kSharedSourceCacheLiveBufferCount
            ? kSharedSourceCacheLiveBufferCount
            : 1;
    result.total_bytes = bytes_per_frame * static_cast<uint64_t>(result.depth);
    result.frozen_snapshot = result.depth == 1;
    result.allowed = result.total_bytes <= budget_bytes;
    return result;
}

} // namespace vr
