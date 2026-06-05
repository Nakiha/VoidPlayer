#include "renderer/playback/renderer_timeline_controller.h"

namespace vr {

RendererTimelineController::RendererTimelineController(
    std::chrono::milliseconds paused_hevc_seek_settle_delay)
    : owned_playback_(std::make_unique<PlaybackController>(create_default_audio_output))
    , playback_(owned_playback_.get())
    , audio_(std::make_unique<AudioCoordinator>(*playback_))
    , seek_(std::make_unique<SeekCoordinator>(paused_hevc_seek_settle_delay)) {}

RendererTimelineController::RendererTimelineController(
    PlaybackController& playback,
    std::chrono::milliseconds paused_hevc_seek_settle_delay)
    : playback_(&playback)
    , audio_(std::make_unique<AudioCoordinator>(*playback_))
    , seek_(std::make_unique<SeekCoordinator>(paused_hevc_seek_settle_delay)) {}

PlaybackController& RendererTimelineController::playback() {
    return *playback_;
}

const PlaybackController& RendererTimelineController::playback() const {
    return *playback_;
}

AudioCoordinator* RendererTimelineController::audio() {
    return audio_.get();
}

const AudioCoordinator* RendererTimelineController::audio() const {
    return audio_.get();
}

SeekCoordinator* RendererTimelineController::seek() {
    return seek_.get();
}

const SeekCoordinator* RendererTimelineController::seek() const {
    return seek_.get();
}

const LoopRangeState& RendererTimelineController::loop_range() const {
    return loop_range_;
}

bool RendererTimelineController::playing() const {
    return playing_.load(std::memory_order_acquire);
}

void RendererTimelineController::set_playing(bool playing) {
    playing_.store(playing, std::memory_order_release);
}

void RendererTimelineController::set_loop_range(LoopRangeState state) {
    loop_range_ = state;
}

void RendererTimelineController::reset_loop_range() {
    loop_range_ = LoopRangeState();
}

RendererLoopRangeSeekDecision RendererTimelineController::evaluate_loop_range_seek(
    bool playing) const {
    const int64_t pts = playback().clock().current_pts_us();
    LoopRangeSeekInput input;
    input.playing = playing;
    input.loop_enabled = loop_range_.enabled;
    input.clock_paused = playback().clock().is_paused();
    input.current_pts_us = pts;
    input.start_us = loop_range_.start_us;
    input.end_us = loop_range_.end_us;
    const auto decision = choose_loop_range_seek(input);
    return RendererLoopRangeSeekDecision{
        decision.should_seek,
        pts,
        decision.target_pts_us,
    };
}

SeekTargetResolution RendererTimelineController::resolve_seek_target(
    int64_t requested_pts_us,
    int64_t effective_duration_us) {
    auto resolution = vr::resolve_seek_target(
        requested_pts_us,
        effective_duration_us,
        pending_seek_preview_event_state());
    if (resolution.retarget_pending_event) {
        retarget_pending_seek_preview_event(resolution.target_pts_us);
    }
    return resolution;
}

RendererSeekPreparationResult RendererTimelineController::prepare_seek(
    int64_t requested_pts_us,
    SeekType type,
    int64_t effective_duration_us,
    bool allow_deferred,
    bool playing,
    bool has_hevc_hw_track) {
    RendererSeekPreparationResult result;
    result.target = resolve_seek_target(requested_pts_us, effective_duration_us);
    RendererSeekClockGateRequest clock_gate_request;
    clock_gate_request.allow_deferred = allow_deferred;
    clock_gate_request.playing = playing;
    clock_gate_request.has_hevc_hw_track = has_hevc_hw_track;
    clock_gate_request.target_pts_us = result.target.target_pts_us;
    clock_gate_request.type = type;
    result.clock_gate = apply_seek_clock_gate(clock_gate_request);
    return result;
}

RendererSeekClockGateResult RendererTimelineController::apply_seek_clock_gate(
    const RendererSeekClockGateRequest& request) {
    RendererSeekClockGateInput input;
    input.allow_deferred = request.allow_deferred;
    input.playing = request.playing;
    input.has_hevc_hw_track = request.has_hevc_hw_track;
    input.target_pts_us = request.target_pts_us;
    input.type = request.type;

    RendererSeekClockGateResult result;
    result.plan = plan_renderer_seek_clock_gate(input);
    if (result.plan.seek_clock) {
        playback().seek_clock(result.plan.target_pts_us);
    }
    if (result.plan.evaluate_paused_hevc_defer && seek_) {
        result.deferred = seek_->should_defer_paused_hevc_seek(
            result.plan.playing,
            result.plan.has_hevc_hw_track,
            result.plan.target_pts_us,
            result.plan.type);
    }
    return result;
}

std::optional<SeekRequest> RendererTimelineController::take_deferred_paused_hevc_seek(
    bool playing) {
    return seek_ ? seek_->take_deferred_paused_hevc_seek(playing) : std::nullopt;
}

RendererPausedHevcPreviewMarkResult
RendererTimelineController::mark_paused_hevc_preview_drawn(
    bool has_hevc_hw_track) {
    RendererPausedHevcPreviewMarkResult result;
    if (!seek_) {
        return result;
    }
    result.was_in_flight = seek_->paused_hevc_seek_in_flight();
    seek_->mark_paused_hevc_preview_drawn(has_hevc_hw_track);
    result.in_flight = seek_->paused_hevc_seek_in_flight();
    return result;
}

void RendererTimelineController::begin_pending_seek_preview_event(
    int64_t request_id,
    int64_t target_pts_us) {
    pending_seek_event_request_id_ = request_id;
    pending_seek_event_target_pts_us_ = target_pts_us;
    pending_seek_event_emitted_ = false;
}

PendingSeekPreviewEventState
RendererTimelineController::pending_seek_preview_event_state() const {
    return PendingSeekPreviewEventState{
        pending_seek_event_request_id_ >= 0,
        pending_seek_event_emitted_,
        pending_seek_event_target_pts_us_,
    };
}

void RendererTimelineController::retarget_pending_seek_preview_event(
    int64_t target_pts_us) {
    pending_seek_event_target_pts_us_ = target_pts_us;
}

std::optional<PendingSeekPreviewEvent>
RendererTimelineController::mark_pending_seek_preview_event_emitted() {
    if (pending_seek_event_request_id_ < 0 || pending_seek_event_emitted_) {
        return std::nullopt;
    }
    pending_seek_event_emitted_ = true;
    return PendingSeekPreviewEvent{
        pending_seek_event_request_id_,
        pending_seek_event_target_pts_us_,
    };
}

void RendererTimelineController::start_session_if_needed() {
    session_started_by_renderer_ = false;
    if (playback_ && !playback_->audio_output()) {
        playback_->start_session();
        session_started_by_renderer_ = true;
    }
}

void RendererTimelineController::stop_session_if_started() {
    if (playback_ && session_started_by_renderer_) {
        playback_->stop_session();
        session_started_by_renderer_ = false;
    }
}

void RendererTimelineController::reset_playback_state() {
    set_playing(false);
}

} // namespace vr
