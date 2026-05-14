#pragma once
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/clock.h"
#include <array>
#include <memory>
#include <mutex>
#include <optional>

namespace vr {

constexpr int64_t kRenderSinkPtsToleranceUs = 5000;
static constexpr size_t kMaxTracks = 4;

struct PresentDecision {
    bool should_present = false;
    std::array<std::optional<TextureFrame>, kMaxTracks> frames;
    int64_t current_pts_us = 0;
};

class RenderSink {
public:
    explicit RenderSink(Clock& clock);

    /// Set or clear a track buffer at a specific slot. RenderSink keeps shared
    /// ownership and evaluate() snapshots handles under its own mutex, so track
    /// removal/compaction cannot leave an in-flight decision with dangling
    /// buffer pointers.
    void set_track(size_t slot, std::shared_ptr<TrackBuffer> track);

    void remove_all_tracks();

    /// Set per-track sync offset in microseconds.
    void set_track_offset(size_t slot, int64_t offset_us);

    PresentDecision evaluate();

private:
    Clock& clock_;
    mutable std::mutex mutex_;
    std::array<std::shared_ptr<TrackBuffer>, kMaxTracks> tracks_{};
    std::array<int64_t, kMaxTracks> track_offsets_{};
};

} // namespace vr
