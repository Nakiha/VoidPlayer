#include "video_renderer/renderer_seek_log_policy.h"

namespace vr {

namespace {

double to_seconds(int64_t pts_us) {
    return pts_us / 1e6;
}

const char* seek_type_label(SeekType type) {
    return type == SeekType::Exact ? "Exact" : "Keyframe";
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

} // namespace vr
