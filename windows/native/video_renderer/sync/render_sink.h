#pragma once
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/clock.h"
#include <vector>
#include <optional>

namespace vr {

constexpr int64_t PTS_TOLERANCE_US = 5000;

struct PresentDecision {
    bool should_present = false;
    std::vector<std::optional<TextureFrame>> frames;
    int64_t current_pts_us = 0;
};

class RenderSink {
public:
    explicit RenderSink(Clock& clock);

    void add_track(TrackBuffer* track);
    void remove_all_tracks();

    PresentDecision evaluate();

private:
    Clock& clock_;
    std::vector<TrackBuffer*> tracks_;
};

} // namespace vr
