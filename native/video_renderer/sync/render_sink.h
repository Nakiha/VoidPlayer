#pragma once
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/clock.h"
#include <array>
#include <optional>

namespace vr {

constexpr int64_t PTS_TOLERANCE_US = 5000;
static constexpr size_t kMaxTracks = 4;

struct PresentDecision {
    bool should_present = false;
    std::array<std::optional<TextureFrame>, kMaxTracks> frames;
    int64_t current_pts_us = 0;
};

class RenderSink {
public:
    explicit RenderSink(Clock& clock);

    /// Set or clear a track buffer at a specific slot.
    /// Pass nullptr to clear a slot.
    void set_track(size_t slot, TrackBuffer* track);

    void remove_all_tracks();

    /// Set per-track sync offset in microseconds.
    void set_track_offset(size_t slot, int64_t offset_us);

    PresentDecision evaluate();

private:
    Clock& clock_;
    std::array<TrackBuffer*, kMaxTracks> tracks_{};
    std::array<int64_t, kMaxTracks> track_offsets_{};
};

} // namespace vr
