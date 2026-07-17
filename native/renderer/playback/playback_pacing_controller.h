#pragma once

#include <cstddef>
#include <cstdint>

namespace vr {

enum class PlaybackPacingState {
    Idle,
    Preroll,
    Running,
    Rebuffering,
};

struct PlaybackPacingSnapshot {
    bool has_active_tracks = false;
    bool preroll_blocked = false;
    bool starvation_risk = false;
    bool resume_ready = false;
    bool frontier_limited = false;
    bool all_active_tracks_eof = false;
    int bottleneck_slot = -1;
    size_t min_buffered_frames = 0;
    int64_t safe_frontier_us = 0;
    int64_t headroom_us = 0;
    int64_t high_watermark_us = 0;
};

struct PlaybackPacingInput {
    bool playing = false;
    bool pacing_held = false;
    bool allow_adaptive_speed = true;
    double requested_speed = 1.0;
    int64_t monotonic_time_us = 0;
    PlaybackPacingSnapshot snapshot;
};

struct PlaybackPacingDecision {
    PlaybackPacingState state = PlaybackPacingState::Idle;
    bool hold_for_pacing = false;
    bool release_pacing_hold = false;
    bool resume_decode = false;
    bool update_effective_speed = false;
    bool entered_preroll = false;
    bool entered_rebuffering = false;
    bool resumed_running = false;
    double effective_speed = 1.0;
};

struct PlaybackPacingDiagnostics {
    PlaybackPacingState state = PlaybackPacingState::Idle;
    double requested_speed = 1.0;
    double effective_speed = 1.0;
    int bottleneck_slot = -1;
    size_t min_buffered_frames = 0;
    int64_t safe_frontier_us = 0;
    int64_t headroom_us = 0;
    uint64_t rebuffer_count = 0;
    uint64_t rebuffer_duration_us = 0;
    uint64_t presentation_skipped_frame_count = 0;
};

// Shared playback admission policy. Track code supplies only immutable PTS
// frontier facts; this controller owns hysteresis and effective clock rate.
// It never mutates track buffers or presentation state.
class PlaybackPacingController {
public:
    PlaybackPacingDecision evaluate(const PlaybackPacingInput& input);
    void reset();
    PlaybackPacingDiagnostics diagnostics() const { return diagnostics_; }

    PlaybackPacingState state() const { return state_; }
    double effective_speed() const { return effective_speed_; }

private:
    static double choose_effective_speed(
        double requested_speed,
        bool allow_adaptive_speed,
        const PlaybackPacingSnapshot& snapshot);
    bool update_effective_speed(double speed,
                                PlaybackPacingDecision& decision);

    PlaybackPacingState state_ = PlaybackPacingState::Idle;
    double effective_speed_ = 1.0;
    int64_t rebuffer_started_us_ = 0;
    PlaybackPacingDiagnostics diagnostics_;
};

const char* playback_pacing_state_name(PlaybackPacingState state);

} // namespace vr
