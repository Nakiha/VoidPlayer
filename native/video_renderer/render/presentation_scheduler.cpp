#include "video_renderer/render/presentation_scheduler.h"

#include <limits>

namespace vr {
namespace {

bool first_presented_frame_pts(const PresentDecision& decision, int64_t& pts_us) {
    if (!decision.should_present) {
        return false;
    }
    for (const auto& frame : decision.frames) {
        if (frame.has_value()) {
            pts_us = frame->pts_us;
            return true;
        }
    }
    return false;
}

} // namespace

void PresentationScheduler::reset() {
    last_presented_pts_us_ = kNoTimestampUs;
}

PresentationSchedulerTick PresentationScheduler::tick(RenderSink& render_sink) {
    PresentationSchedulerTick result;
    result.decision = render_sink.evaluate();

    int64_t pts_us = 0;
    if (!first_presented_frame_pts(result.decision, pts_us)) {
        return result;
    }

    result.has_presentable_frame = true;
    result.selected_pts_us = pts_us;
    if (pts_us == last_presented_pts_us_) {
        return result;
    }

    last_presented_pts_us_ = pts_us;
    result.should_notify = true;
    return result;
}

bool PresentationScheduler::advance_to_clock(RenderSink& render_sink,
                                             int64_t* selected_pts_us) const {
    const auto decision = render_sink.evaluate();
    int64_t pts_us = 0;
    if (!first_presented_frame_pts(decision, pts_us)) {
        return false;
    }
    if (selected_pts_us) {
        *selected_pts_us = pts_us;
    }
    return true;
}

} // namespace vr
