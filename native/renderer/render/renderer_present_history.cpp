#include "renderer/render/renderer_present_history.h"

#include "renderer/track/track_lifecycle.h"
#include "renderer/track/track_present_policy.h"

#include <optional>
#include <utility>

namespace vr {

void RendererPresentHistory::reset() {
    decision_ = PresentDecision();
}

PresentDecision RendererPresentHistory::snapshot() const {
    return decision_;
}

void RendererPresentHistory::set(PresentDecision decision) {
    decision_ = std::move(decision);
}

void RendererPresentHistory::clear_slot(size_t slot) {
    clear_present_decision_slot(decision_, slot);
}

void RendererPresentHistory::clear_reserved_slot(size_t slot) {
    if (slot >= kMaxTracks) {
        return;
    }
    decision_.frames[slot] = std::nullopt;
    decision_.file_ids[slot] = -1;
    decision_.track_generations[slot] = 0;
    decision_.should_present = present_decision_has_frame(decision_);
}

void RendererPresentHistory::compact_from(size_t slot) {
    compact_present_decision_frames(decision_, slot);
}

} // namespace vr
