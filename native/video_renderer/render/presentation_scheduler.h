#pragma once

#include "video_renderer/sync/render_sink.h"

#include <cstdint>

namespace vr {

struct PresentationSchedulerTick {
    bool should_notify = false;
    bool has_presentable_frame = false;
    int64_t selected_pts_us = 0;
    PresentDecision decision;
};

class PresentationScheduler {
public:
    void reset();

    PresentationSchedulerTick tick(RenderSink& render_sink);
    bool advance_to_clock(RenderSink& render_sink, int64_t* selected_pts_us = nullptr) const;

private:
    int64_t last_presented_pts_us_ = kNoTimestampUs;
};

} // namespace vr
