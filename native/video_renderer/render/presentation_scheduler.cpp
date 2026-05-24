#include "video_renderer/render/presentation_scheduler.h"

#include <cstddef>
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

PresentationScheduler::PresentedSignature PresentationScheduler::signature_for(
    const PresentDecision& decision) {
    PresentedSignature signature;
    signature.should_present = decision.should_present;
    signature.file_ids = decision.file_ids;
    signature.track_generations = decision.track_generations;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        signature.has_frame[i] = decision.frames[i].has_value();
        signature.pts_us[i] =
            decision.frames[i].has_value() ? decision.frames[i]->pts_us : kNoTimestampUs;
    }
    return signature;
}

void PresentationScheduler::reset() {
    last_presented_signature_ = {};
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
    const auto signature = signature_for(result.decision);
    if (signature.should_present == last_presented_signature_.should_present &&
        signature.has_frame == last_presented_signature_.has_frame &&
        signature.pts_us == last_presented_signature_.pts_us &&
        signature.file_ids == last_presented_signature_.file_ids &&
        signature.track_generations == last_presented_signature_.track_generations) {
        return result;
    }

    last_presented_signature_ = signature;
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
