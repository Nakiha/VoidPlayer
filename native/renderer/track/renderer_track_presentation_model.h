#pragma once

#include "renderer/track/renderer_track_types.h"
#include "renderer/track/track_perf_baseline.h"
#include "renderer/track/track_present_policy.h"
#include "renderer/track/track_preview_policy.h"
#include "renderer/track/track_snapshot.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace vr {

class RendererTrackRegistry;

// Builds immutable track snapshots and present decisions from the registry.
// Runtime performance baselines stay internal to this model.
class RendererTrackPresentationModel {
public:
    explicit RendererTrackPresentationModel(RendererTrackRegistry& registry);

    std::vector<TrackInfo> infos() const;
    std::vector<TrackPerfStats> perf_stats(
        const PresentDecision& last_decision,
        std::chrono::steady_clock::time_point now);
    void reset_perf_baseline(std::chrono::steady_clock::time_point now);
    void reset_perf_baseline();

    TrackGpuMemoryStatsCollectionResult gpu_memory_stats(
        const std::array<uint64_t, kMaxTracks>&
            presenter_copy_texture_bytes_by_slot) const;
    LayoutTrackGeometryList layout_track_geometry() const;
    void populate_draw_tracks(RendererDrawTrackSnapshotList& out) const;
    std::vector<RendererLayoutTrackReference> layout_track_references() const;
    std::vector<RenderLoopTrackDiagnosticSnapshot>
    render_loop_diagnostics() const;

    StepDecisionBuildResult build_step_forward_decision(
        int64_t current_pts_us,
        const PresentDecision& last_decision,
        PresentDecision& decision) const;
    StepForwardExactSeekTarget choose_step_forward_exact_seek_target(
        int64_t clock_pts_us,
        const PresentDecision& last_decision,
        std::optional<int64_t> logical_step_anchor_us = std::nullopt) const;
    bool build_step_backward_decision(int64_t current_pts_us,
                                      const PresentDecision& last_decision,
                                      PresentDecision& decision) const;
    StepBackwardExactSeekTarget choose_step_backward_exact_seek_target(
        int64_t clock_pts_us,
        const PresentDecision& last_decision) const;
    void apply_carry_forward(const PresentDecision& last_decision,
                             PresentDecision& decision) const;
    void filter_present_decision(PresentDecision& decision) const;
    std::vector<SeekPreviewPresentedTrackEvent> collect_seek_preview_presented_events(
        const PresentDecision& decision,
        int64_t request_id,
        int64_t target_pts_us) const;
    std::vector<LayoutTrackGeometryUpdate> update_layout_track_geometry_from_decision(
        const PresentDecision& decision);
    EmptyBufferEofClamp empty_buffer_eof_clamp(
        const PresentDecision& last_decision) const;
    std::optional<int64_t> next_frame_event_pts_us(int64_t current_pts_us) const;
    RendererPausedCachedDecision paused_cached_decision(
        const PresentDecision& last_decision) const;
    RendererPausedLayoutDecision paused_layout_decision(
        const PresentDecision& last_decision) const;
    RendererPausedRefreshDecision paused_refresh_decision(
        const PresentDecision& last_decision,
        const std::optional<PresentDecision>& evaluated_decision,
        bool decoded_preview_refresh) const;
    bool has_complete_cached_decision(
        const PresentDecision& last_decision) const;
    PausedPreviewSnapshot paused_preview_snapshot() const;
    RendererTrackReferenceSnapshot first_active_reference() const;

private:
    RendererTrackRegistry& registry_;
    TrackPerfBaselineTracker perf_baseline_tracker_;
};

} // namespace vr
