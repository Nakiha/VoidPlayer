#pragma once

#include "audio/audio_output_factory.h"
#include "playback/playback_controller.h"
#include "renderer/audio_coordinator.h"
#include "renderer/seek/seek_coordinator.h"

#include <chrono>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

namespace vr {

struct PendingSeekPreviewEvent {
    int64_t request_id = -1;
    int64_t target_pts_us = -1;
};

struct RendererLoopRangeSeekDecision {
    bool should_seek = false;
    int64_t current_pts_us = 0;
    int64_t target_pts_us = 0;
};

struct RendererSeekClockGateRequest {
    bool allow_deferred = true;
    bool playing = false;
    bool has_hevc_hw_track = false;
    int64_t target_pts_us = 0;
    SeekType type = SeekType::Keyframe;
};

struct RendererSeekClockGateResult {
    RendererSeekClockGatePlan plan;
    bool deferred = false;
};

struct RendererSeekPreparationResult {
    SeekTargetResolution target;
    RendererSeekClockGateResult clock_gate;
};

struct RendererPausedHevcPreviewMarkResult {
    bool was_in_flight = false;
    bool in_flight = false;
};

// Lock contract:
// - Owns playback controller lifetime, audio coordinator, seek coordinator,
//   and whether Renderer started the playback session.
// - Does not take renderer state/device/texture locks and does not call
//   presentation/platform callbacks.
class RendererTimelineController {
public:
    explicit RendererTimelineController(std::chrono::milliseconds paused_hevc_seek_settle_delay);
    RendererTimelineController(PlaybackController& playback,
                               std::chrono::milliseconds paused_hevc_seek_settle_delay);

    PlaybackController& playback();
    const PlaybackController& playback() const;
    AudioCoordinator* audio();
    const AudioCoordinator* audio() const;
    SeekCoordinator* seek();
    const SeekCoordinator* seek() const;

    const LoopRangeState& loop_range() const;
    bool playing() const;
    void set_playing(bool playing);
    void set_loop_range(LoopRangeState state);
    void reset_loop_range();
    RendererLoopRangeSeekDecision evaluate_loop_range_seek(bool playing) const;
    SeekTargetResolution resolve_seek_target(int64_t requested_pts_us,
                                             int64_t effective_duration_us);
    RendererSeekPreparationResult prepare_seek(int64_t requested_pts_us,
                                               SeekType type,
                                               int64_t effective_duration_us,
                                               bool allow_deferred,
                                               bool playing,
                                               bool has_hevc_hw_track);
    RendererSeekClockGateResult apply_seek_clock_gate(
        const RendererSeekClockGateRequest& request);
    std::optional<SeekRequest> take_deferred_paused_hevc_seek(bool playing);
    RendererPausedHevcPreviewMarkResult mark_paused_hevc_preview_drawn(
        bool has_hevc_hw_track);

    void begin_pending_seek_preview_event(int64_t request_id, int64_t target_pts_us);
    PendingSeekPreviewEventState pending_seek_preview_event_state() const;
    void retarget_pending_seek_preview_event(int64_t target_pts_us);
    std::optional<PendingSeekPreviewEvent> mark_pending_seek_preview_event_emitted();

    void start_session_if_needed();
    void stop_session_if_started();
    void reset_playback_state();

private:
    std::unique_ptr<PlaybackController> owned_playback_;
    PlaybackController* playback_ = nullptr;
    bool session_started_by_renderer_ = false;
    std::unique_ptr<AudioCoordinator> audio_;
    std::unique_ptr<SeekCoordinator> seek_;
    LoopRangeState loop_range_;
    int64_t pending_seek_event_request_id_ = -1;
    int64_t pending_seek_event_target_pts_us_ = -1;
    bool pending_seek_event_emitted_ = true;
    std::atomic<bool> playing_{false};
};

} // namespace vr
