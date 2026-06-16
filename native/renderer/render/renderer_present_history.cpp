#include "renderer/render/renderer_present_history.h"

#include "renderer/track/track_lifecycle.h"
#include "renderer/track/track_present_policy.h"

#include <optional>
#include <utility>

namespace vr {
namespace {

bool same_presented_anchor_decision(const PresentDecision& a,
                                    const PresentDecision& b) {
    if (a.should_present != b.should_present ||
        a.current_pts_us != b.current_pts_us ||
        a.file_ids != b.file_ids ||
        a.track_generations != b.track_generations) {
        return false;
    }
    for (size_t slot = 0; slot < kMaxTracks; ++slot) {
        if (a.frames[slot].has_value() != b.frames[slot].has_value()) {
            return false;
        }
        if (!a.frames[slot].has_value()) {
            continue;
        }
        const auto& lhs = a.frames[slot].value();
        const auto& rhs = b.frames[slot].value();
        if (lhs.pts_us != rhs.pts_us ||
            lhs.dts_us != rhs.dts_us ||
            lhs.analysis_frame_index != rhs.analysis_frame_index ||
            lhs.source_packet_index != rhs.source_packet_index ||
            lhs.source_packet_pos != rhs.source_packet_pos ||
            lhs.frame_identity_mode != rhs.frame_identity_mode) {
            return false;
        }
    }
    return true;
}

} // namespace

void RendererPresentHistory::reset() {
    decision_ = PresentDecision();
    diagnostics_.mode = RendererPresentedAnchorMode::None;
    diagnostics_.generation++;
    diagnostics_.source_cache_ring_generation = 0;
    diagnostics_.source_cache_frame_generation = 0;
    diagnostics_.update_count++;
    diagnostics_.stale_after_seek = false;
}

PresentDecision RendererPresentHistory::snapshot() const {
    return decision_;
}

RendererPresentedAnchorDiagnostics RendererPresentHistory::diagnostics() const {
    return diagnostics_;
}

void RendererPresentHistory::set(PresentDecision decision) {
    if (diagnostics_.mode == RendererPresentedAnchorMode::SourceCachePublish &&
        same_presented_anchor_decision(decision_, decision)) {
        decision_ = std::move(decision);
        return;
    }
    decision_ = std::move(decision);
    diagnostics_.mode = RendererPresentedAnchorMode::ViewportPresent;
    diagnostics_.generation++;
    diagnostics_.source_cache_ring_generation = 0;
    diagnostics_.source_cache_frame_generation = 0;
    diagnostics_.update_count++;
    diagnostics_.stale_after_seek = false;
}

void RendererPresentHistory::set_source_cache_published(
    PresentDecision decision,
    uint64_t ring_generation,
    uint64_t frame_generation) {
    decision_ = std::move(decision);
    diagnostics_.mode = RendererPresentedAnchorMode::SourceCachePublish;
    diagnostics_.generation++;
    diagnostics_.source_cache_ring_generation = ring_generation;
    diagnostics_.source_cache_frame_generation = frame_generation;
    diagnostics_.update_count++;
    diagnostics_.source_cache_publish_count++;
    diagnostics_.stale_after_seek = false;
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
