#include "windows/shared/shared_texture_ring_types.h"

namespace vr {

SourceCacheRingPolicy resolve_source_cache_ring_policy(
    const std::vector<SourceCacheTrackDescriptor>& descriptors,
    uint64_t budget_bytes) {
    return resolve_source_compositor_ring_policy(descriptors, budget_bytes);
}

} // namespace vr
