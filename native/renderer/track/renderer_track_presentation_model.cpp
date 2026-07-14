#include "renderer/track/renderer_track_presentation_model.h"

#include "renderer/track/renderer_track_registry.h"
#include "renderer/track/track_present_policy.h"
#include "renderer/track/track_preview_policy.h"
#include "renderer/track/track_snapshot.h"
#include "renderer/track/track_step_policy.h"

namespace vr {

RendererTrackPresentationModel::RendererTrackPresentationModel(
    RendererTrackRegistry& registry)
    : registry_(registry) {}

std::vector<TrackInfo> RendererTrackPresentationModel::infos() const {
    return snapshot_track_infos(registry_.tracks_for_snapshot());
}

std::vector<TrackPerfStats> RendererTrackPresentationModel::perf_stats(
    const PresentDecision& last_decision,
    std::chrono::steady_clock::time_point now) {
    const auto& tracks = registry_.tracks_for_snapshot();
    const double elapsed_s = perf_baseline_tracker_.elapsed_seconds(now);
    const bool should_rotate =
        perf_baseline_tracker_.should_rotate(elapsed_s);
    PresentDecision filtered_last_decision = last_decision;
    filter_present_decision_against_tracks(filtered_last_decision, tracks);
    const auto snapshot = snapshot_track_perf_stats_collection(
        tracks,
        filtered_last_decision,
        perf_baseline_tracker_,
        elapsed_s);

    if (should_rotate) {
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!snapshot.frames_decoded_by_slot[i].has_value()) {
                continue;
            }
            perf_baseline_tracker_.update_baseline_frames(
                i, *snapshot.frames_decoded_by_slot[i]);
        }
        perf_baseline_tracker_.rotate_timer(now);
    }
    return snapshot.stats;
}

void RendererTrackPresentationModel::reset_perf_baseline(
    std::chrono::steady_clock::time_point now) {
    perf_baseline_tracker_.reset(now);
}

void RendererTrackPresentationModel::reset_perf_baseline() {
    perf_baseline_tracker_.reset();
}

TrackGpuMemoryStatsCollectionResult
RendererTrackPresentationModel::gpu_memory_stats(
    const std::array<uint64_t, kMaxTracks>&
        presenter_copy_texture_bytes_by_slot) const {
    return snapshot_track_gpu_memory_stats_collection(
        registry_.tracks_for_snapshot(), presenter_copy_texture_bytes_by_slot);
}

LayoutTrackGeometryList RendererTrackPresentationModel::layout_track_geometry() const {
    return snapshot_layout_track_geometry(registry_.tracks_for_snapshot());
}

void RendererTrackPresentationModel::populate_draw_tracks(
    RendererDrawTrackSnapshotList& out) const {
    const auto& tracks = registry_.tracks_for_snapshot();
    out = {};
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        auto& track = out[i];
        track.active = true;
        track.file_id = tracks[i]->file_id;
        track.generation = tracks[i]->generation;
        track.offset_us = tracks[i]->offset_us;
        track.video_width = tracks[i]->video_width;
        track.video_height = tracks[i]->video_height;
        track.video_aspect = tracks[i]->video_aspect;
    }
}

std::vector<RendererLayoutTrackReference>
RendererTrackPresentationModel::layout_track_references() const {
    const auto& tracks = registry_.tracks_for_snapshot();
    std::vector<RendererLayoutTrackReference> refs;
    refs.reserve(registry_.count());
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }
        refs.push_back(RendererLayoutTrackReference{
            tracks[i]->file_id,
            static_cast<int>(i),
        });
    }
    return refs;
}

std::vector<RenderLoopTrackDiagnosticSnapshot>
RendererTrackPresentationModel::render_loop_diagnostics() const {
    return snapshot_render_loop_track_diagnostics(registry_.tracks_for_snapshot());
}

StepDecisionBuildResult
RendererTrackPresentationModel::build_step_forward_decision(
    int64_t current_pts_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    return build_step_forward_decision_result(
        registry_.tracks_for_snapshot(),
        current_pts_us,
        registry_.min_current_frame_duration_us(),
        last_decision,
        decision);
}

StepForwardExactSeekTarget
RendererTrackPresentationModel::choose_step_forward_exact_seek_target(
    int64_t clock_pts_us,
    const PresentDecision& last_decision,
    std::optional<int64_t> logical_step_anchor_us) const {
    return vr::choose_step_forward_exact_seek_target(
        registry_.tracks_for_snapshot(),
        clock_pts_us,
        registry_.cached_duration(),
        last_decision,
        logical_step_anchor_us);
}

bool RendererTrackPresentationModel::build_step_backward_decision(
    int64_t current_pts_us,
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    return vr::build_step_backward_decision(
        registry_.tracks_for_snapshot(), current_pts_us, last_decision, decision);
}

StepBackwardExactSeekTarget
RendererTrackPresentationModel::choose_step_backward_exact_seek_target(
    int64_t clock_pts_us,
    const PresentDecision& last_decision) const {
    return vr::choose_step_backward_exact_seek_target(
        registry_.tracks_for_snapshot(), clock_pts_us, last_decision);
}

StepBackwardReconstructionPlan
RendererTrackPresentationModel::build_step_backward_reconstruction_plan(
    int64_t clock_pts_us,
    const PresentDecision& last_decision) const {
    return vr::build_step_backward_reconstruction_plan(
        registry_.tracks_for_snapshot(), clock_pts_us, last_decision);
}

void RendererTrackPresentationModel::apply_carry_forward(
    const PresentDecision& last_decision,
    PresentDecision& decision) const {
    apply_present_carry_forward(
        registry_.tracks_for_snapshot(), last_decision, decision);
}

void RendererTrackPresentationModel::filter_present_decision(
    PresentDecision& decision) const {
    filter_present_decision_against_tracks(
        decision, registry_.tracks_for_snapshot());
}

std::vector<SeekPreviewPresentedTrackEvent>
RendererTrackPresentationModel::collect_seek_preview_presented_events(
    const PresentDecision& decision,
    int64_t request_id,
    int64_t target_pts_us) const {
    return collect_seek_preview_presented_track_events(
        registry_.tracks_for_snapshot(), decision, request_id, target_pts_us);
}

std::vector<LayoutTrackGeometryUpdate>
RendererTrackPresentationModel::update_layout_track_geometry_from_decision(
    const PresentDecision& decision) {
    return registry_.update_layout_track_geometry_from_decision(decision);
}

EmptyBufferEofClamp RendererTrackPresentationModel::empty_buffer_eof_clamp(
    const PresentDecision& last_decision) const {
    return compute_empty_buffer_eof_clamp(
        registry_.tracks_for_snapshot(), last_decision);
}

std::optional<int64_t> RendererTrackPresentationModel::next_frame_event_pts_us(
    int64_t current_pts_us) const {
    return compute_next_frame_event_pts_us(
        registry_.tracks_for_snapshot(), current_pts_us);
}

RendererPausedCachedDecision
RendererTrackPresentationModel::paused_cached_decision(
    const PresentDecision& last_decision) const {
    const auto& tracks = registry_.tracks_for_snapshot();
    RendererPausedCachedDecision result;
    PresentDecision decision = last_decision;
    filter_present_decision_against_tracks(decision, tracks);
    if (!present_decision_has_frame(decision)) {
        return result;
    }

    const auto available = build_available_paused_frame_snapshot(tracks);
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!decision.frames[i].has_value() &&
            available.decision.frames[i].has_value()) {
            decision.frames[i] = available.decision.frames[i];
            decision.file_ids[i] = available.decision.file_ids[i];
            decision.track_generations[i] =
                available.decision.track_generations[i];
        }
    }
    filter_present_decision_against_tracks(decision, tracks);

    result.has_frame = present_decision_has_frame(decision);
    if (result.has_frame) {
        result.decision = decision;
        result.first_pts_us = first_present_decision_frame_pts_us(decision);
    }
    return result;
}

RendererPausedLayoutDecision
RendererTrackPresentationModel::paused_layout_decision(
    const PresentDecision& last_decision) const {
    const auto& tracks = registry_.tracks_for_snapshot();
    RendererPausedLayoutDecision result;
    result.active_track_count = active_track_count(tracks);

    const auto cached = paused_cached_decision(last_decision);
    if (!cached.has_frame) {
        return result;
    }

    result.decision = cached.decision;
    result.has_frame = true;
    if (result.active_track_count > 1 &&
        !present_decision_covers_active_tracks(result.decision, tracks)) {
        const auto preview = paused_preview_snapshot();
        if (preview.ready_to_present) {
            result.decision = preview.decision;
        } else {
            result.has_frame = false;
        }
    }
    return result;
}

RendererPausedRefreshDecision
RendererTrackPresentationModel::paused_refresh_decision(
    const PresentDecision& last_decision,
    const std::optional<PresentDecision>& evaluated_decision,
    bool decoded_preview_refresh) const {
    const auto& tracks = registry_.tracks_for_snapshot();
    RendererPausedRefreshDecision result;
    PresentDecision decision;

    if (decoded_preview_refresh) {
        const auto snapshot = paused_preview_snapshot();
        if (snapshot.ready_to_present) {
            decision = snapshot.decision;
            result.has_frame = true;
        } else {
            const auto available = build_available_paused_frame_snapshot(tracks);
            if (available.has_frame) {
                decision = available.decision;
                result.has_frame = true;
            }
        }
    } else if (evaluated_decision.has_value()) {
        decision = *evaluated_decision;
        filter_present_decision_against_tracks(decision, tracks);
        if (decision.should_present) {
            apply_present_carry_forward(tracks, last_decision, decision);
        }
        result.has_frame = present_decision_has_frame(decision);
    }

    if (!decoded_preview_refresh &&
        !result.has_frame &&
        present_decision_has_frame(last_decision)) {
        decision = last_decision;
        result.has_frame = true;
    }

    if (result.has_frame) {
        const auto available = build_available_paused_frame_snapshot(tracks);
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!decision.frames[i].has_value() &&
                available.decision.frames[i].has_value()) {
                decision.frames[i] = available.decision.frames[i];
                decision.file_ids[i] = available.decision.file_ids[i];
                decision.track_generations[i] =
                    available.decision.track_generations[i];
            }
        }
        filter_present_decision_against_tracks(decision, tracks);
        result.has_frame = present_decision_has_frame(decision);
    }

    if (result.has_frame &&
        active_track_count(tracks) > 1 &&
        !present_decision_covers_active_tracks(decision, tracks)) {
        PresentDecision cached = last_decision;
        filter_present_decision_against_tracks(cached, tracks);
        if (!decoded_preview_refresh &&
            present_decision_covers_active_tracks(cached, tracks)) {
            decision = cached;
        } else if (!decoded_preview_refresh) {
            const auto snapshot = paused_preview_snapshot();
            if (snapshot.ready_to_present) {
                decision = snapshot.decision;
                result.has_frame = true;
            } else {
                result.has_frame = false;
            }
        }
    }

    if (result.has_frame) {
        result.decision = decision;
    }
    return result;
}

bool RendererTrackPresentationModel::has_complete_cached_decision(
    const PresentDecision& last_decision) const {
    const auto& tracks = registry_.tracks_for_snapshot();
    PresentDecision cached = last_decision;
    filter_present_decision_against_tracks(cached, tracks);
    const auto active_count = active_track_count(tracks);
    return present_decision_has_frame(cached) &&
           (active_count <= 1 ||
            present_decision_covers_active_tracks(cached, tracks));
}

PausedPreviewSnapshot RendererTrackPresentationModel::paused_preview_snapshot() const {
    return build_paused_preview_snapshot(registry_.tracks_for_snapshot());
}

RendererTrackReferenceSnapshot
RendererTrackPresentationModel::first_active_reference() const {
    const auto& tracks = registry_.tracks_for_snapshot();
    RendererTrackReferenceSnapshot result;
    result.slot = registry_.first_active_slot();
    if (result.slot >= 0 &&
        tracks[static_cast<size_t>(result.slot)]) {
        result.offset_us =
            tracks[static_cast<size_t>(result.slot)]->offset_us;
    }
    return result;
}

} // namespace vr
