#pragma once

#include "renderer/render/presentation_snapshot.h"

#include <cstddef>
#include <cstdint>

namespace vr {

enum class RendererPresentedAnchorMode {
    None,
    ViewportPresent,
    SourceCachePublish,
};

struct RendererPresentedAnchorDiagnostics {
    RendererPresentedAnchorMode mode = RendererPresentedAnchorMode::None;
    uint64_t generation = 0;
    uint64_t source_cache_ring_generation = 0;
    uint64_t source_cache_frame_generation = 0;
    uint64_t update_count = 0;
    uint64_t source_cache_publish_count = 0;
    bool stale_after_seek = false;
};

// Lock contract:
// - Owns the last presented decision cache.
// - Does not take locks or call callbacks.
// - Callers serialize access with the renderer state lock.
class RendererPresentHistory {
public:
    void reset();
    PresentDecision snapshot() const;
    RendererPresentedAnchorDiagnostics diagnostics() const;
    void set(PresentDecision decision);
    void set_source_cache_published(PresentDecision decision,
                                    uint64_t ring_generation,
                                    uint64_t frame_generation);
    void clear_slot(size_t slot);
    void clear_reserved_slot(size_t slot);
    void compact_from(size_t slot);

private:
    PresentDecision decision_;
    RendererPresentedAnchorDiagnostics diagnostics_;
};

} // namespace vr
