#include "renderer/seek/renderer_seek_log_policy.h"

#include <spdlog/spdlog.h>

namespace vr {

namespace {

double to_seconds(int64_t pts_us) {
    return pts_us / 1e6;
}

const char* seek_type_label(SeekType type) {
    return is_exact_seek_type(type) ? "Exact" : "Keyframe";
}

} // namespace

SeekRequestLogFacts build_seek_request_log_facts(
    int64_t target_pts_us,
    SeekType type) {
    SeekRequestLogFacts facts;
    facts.target_seconds = to_seconds(target_pts_us);
    facts.type_label = seek_type_label(type);
    return facts;
}

SeekClampLogFacts build_seek_clamp_log_facts(
    const SeekTargetResolution& resolution) {
    SeekClampLogFacts facts;
    facts.should_log = resolution.clamped;
    facts.requested_seconds = to_seconds(resolution.requested_pts_us);
    facts.clamped_seconds = to_seconds(resolution.target_pts_us);
    facts.duration_seconds = to_seconds(resolution.effective_duration_us);
    return facts;
}

TrackSeekTargetClampLogFacts build_track_seek_target_clamp_log_facts(
    size_t slot,
    const TrackSeekTargetResolution& target) {
    TrackSeekTargetClampLogFacts facts;
    facts.should_log = target.clamped;
    facts.slot = slot;
    facts.requested_seconds = to_seconds(target.requested_target_us);
    facts.clamped_seconds = to_seconds(target.target_us);
    return facts;
}

TrackSeekCoalescingLogFacts build_track_seek_coalescing_log_facts(
    size_t slot,
    const TrackSeekTargetResolution& target,
    const TrackSeekPreparationResult& preparation,
    const TrackSeekExecutionResult& execution) {
    TrackSeekCoalescingLogFacts facts;
    facts.should_log = execution.coalescing_transition;
    facts.slot = slot;
    facts.buffer_state_before = static_cast<int>(preparation.buffer_state_before);
    facts.target_seconds = to_seconds(target.target_us);
    return facts;
}

TrackSeekClearedLogFacts build_track_seek_cleared_log_facts(
    size_t slot,
    const TrackSeekTargetResolution& target,
    const TrackSeekPreparationResult& preparation,
    const TrackSeekExecutionResult& execution,
    size_t buffered_frames_after) {
    TrackSeekClearedLogFacts facts;
    facts.should_log = execution.applied_seek;
    facts.slot = slot;
    facts.buffered_frames_before = preparation.buffered_frames_before;
    facts.buffered_frames_after = buffered_frames_after;
    facts.packet_queue_size_before = preparation.packet_queue_size_before;
    facts.target_seconds = to_seconds(target.target_us);
    return facts;
}

bool log_renderer_track_seek_application_results(
    const std::vector<RendererTrackSeekApplicationResult>& seek_results) {
    bool applied_seek = false;
    for (const auto& track_seek : seek_results) {
        const size_t slot = track_seek.slot;
        const auto& seek_result = track_seek.seek;
        const auto& seek_facts = seek_result.facts;
        if (seek_facts.warn_h264_flv_exact_seek) {
            spdlog::warn("[Renderer] Exact seek on H.264/FLV is best-effort: "
                         "streams that omit SPS/PPS on IDR frames can decode "
                         "incorrectly after seek. Remux/re-encode with repeated "
                         "headers for frame-accurate previews.");
        }

        const auto& target = seek_facts.target;
        const auto clamp_log =
            build_track_seek_target_clamp_log_facts(slot, target);
        if (clamp_log.should_log) {
            spdlog::info("[Renderer] seek_internal: track[{}] target clamp "
                         "requested={:.3f}s, clamped={:.3f}s",
                         clamp_log.slot,
                         clamp_log.requested_seconds,
                         clamp_log.clamped_seconds);
        }

        const auto& preparation = seek_result.preparation;
        const auto& execution = seek_result.execution;
        const auto coalescing_log = build_track_seek_coalescing_log_facts(
            slot, target, preparation, execution);
        if (coalescing_log.should_log) {
            spdlog::info(
                "[Renderer] seek_internal: track[{}] coalescing HEVC HW seek during transition "
                "(buf_state_before={}, target={:.3f}s)",
                coalescing_log.slot,
                coalescing_log.buffer_state_before,
                coalescing_log.target_seconds);
        }
        if (!execution.applied_seek) {
            continue;
        }

        applied_seek = true;
        const auto cleared_log = build_track_seek_cleared_log_facts(
            slot,
            target,
            preparation,
            execution,
            track_seek.buffered_frames_after);
        spdlog::info(
            "[Renderer] seek_internal: track[{}] cleared (buf={}->{}, pq={}->0), state->Flushing, target={:.3f}s",
            cleared_log.slot,
            cleared_log.buffered_frames_before,
            cleared_log.buffered_frames_after,
            cleared_log.packet_queue_size_before,
            cleared_log.target_seconds);
    }
    return applied_seek;
}

} // namespace vr
