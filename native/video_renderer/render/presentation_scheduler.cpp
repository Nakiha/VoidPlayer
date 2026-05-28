#include "video_renderer/render/presentation_scheduler.h"

#include <cstddef>
#include <limits>

namespace vr {
namespace {

bool first_presented_frame(const PresentDecision& decision,
                           size_t& slot,
                           int64_t& pts_us) {
    if (!decision.should_present) {
        return false;
    }
    for (size_t i = 0; i < decision.frames.size(); ++i) {
        const auto& frame = decision.frames[i];
        if (frame.has_value()) {
            slot = i;
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
    size_t slot = kMaxTracks;
    int64_t pts_us = kNoTimestampUs;
    if (first_presented_frame(decision, slot, pts_us)) {
        signature.reference_slot = slot;
        signature.pts_us = pts_us;
        signature.file_id = decision.file_ids[slot];
        signature.track_generation = decision.track_generations[slot];
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
    size_t slot = kMaxTracks;
    if (!first_presented_frame(result.decision, slot, pts_us)) {
        return result;
    }

    result.has_presentable_frame = true;
    result.selected_pts_us = pts_us;
    const auto signature = signature_for(result.decision);
    if (signature.should_present == last_presented_signature_.should_present &&
        signature.pts_us == last_presented_signature_.pts_us &&
        signature.reference_slot == last_presented_signature_.reference_slot &&
        signature.file_id == last_presented_signature_.file_id &&
        signature.track_generation == last_presented_signature_.track_generation) {
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
    size_t slot = kMaxTracks;
    if (!first_presented_frame(decision, slot, pts_us)) {
        return false;
    }
    if (selected_pts_us) {
        *selected_pts_us = pts_us;
    }
    return true;
}

} // namespace vr
