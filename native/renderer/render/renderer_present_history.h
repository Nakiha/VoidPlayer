#pragma once

#include "renderer/render/presentation_snapshot.h"

#include <cstddef>

namespace vr {

// Lock contract:
// - Owns the last presented decision cache.
// - Does not take locks or call callbacks.
// - Callers serialize access with the renderer state lock.
class RendererPresentHistory {
public:
    void reset();
    PresentDecision snapshot() const;
    void set(PresentDecision decision);
    void clear_slot(size_t slot);
    void clear_reserved_slot(size_t slot);
    void compact_from(size_t slot);

private:
    PresentDecision decision_;
};

} // namespace vr
