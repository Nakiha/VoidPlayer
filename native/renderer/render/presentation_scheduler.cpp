#include "renderer/render/presentation_scheduler.h"

#include <cstddef>
#include <limits>

namespace vr {
namespace {

bool representative_presented_frame(const PresentDecision& decision,
                                    size_t& slot,
                                    int64_t& pts_us) {
    if (!decision.should_present) {
        return false;
    }
    bool found = false;
    for (size_t i = 0; i < decision.frames.size(); ++i) {
        const auto& frame = decision.frames[i];
        if (frame.has_value()) {
            if (!found || frame->pts_us > pts_us) {
                slot = i;
                pts_us = frame->pts_us;
                found = true;
            }
        }
    }
    return found;
}

} // namespace

PresentationScheduler::PresentedSignature PresentationScheduler::signature_for(
    const PresentDecision& decision) {
    PresentedSignature signature;
    signature.should_present = decision.should_present;
    for (size_t slot = 0; slot < kMaxTracks; ++slot) {
        const auto& frame = decision.frames[slot];
        if (!frame.has_value()) {
            continue;
        }
        auto& out = signature.frames[slot];
        out.present = true;
        out.pts_us = frame->pts_us;
        out.dts_us = frame->dts_us;
        out.source_packet_index = frame->source_packet_index;
        out.storage_identity =
            reinterpret_cast<uintptr_t>(frame->texture_handle);
        if (out.storage_identity == 0 && frame->cpu_data) {
            out.storage_identity =
                reinterpret_cast<uintptr_t>(frame->cpu_data.get());
        }
        out.file_id = decision.file_ids[slot];
        out.track_generation = decision.track_generations[slot];
    }
    return signature;
}

bool PresentationScheduler::signatures_equal(
    const PresentedSignature& left,
    const PresentedSignature& right) {
    if (left.should_present != right.should_present) {
        return false;
    }
    for (size_t slot = 0; slot < kMaxTracks; ++slot) {
        const auto& a = left.frames[slot];
        const auto& b = right.frames[slot];
        if (a.present != b.present ||
            a.pts_us != b.pts_us ||
            a.dts_us != b.dts_us ||
            a.source_packet_index != b.source_packet_index ||
            a.storage_identity != b.storage_identity ||
            a.file_id != b.file_id ||
            a.track_generation != b.track_generation) {
            return false;
        }
    }
    return true;
}

void PresentationScheduler::reset() {
    last_presented_signature_ = {};
}

PresentationSchedulerTick PresentationScheduler::tick(RenderSink& render_sink) {
    PresentationSchedulerTick result;
    result.decision = render_sink.evaluate();

    int64_t pts_us = 0;
    size_t slot = kMaxTracks;
    if (!representative_presented_frame(result.decision, slot, pts_us)) {
        return result;
    }

    result.has_presentable_frame = true;
    result.selected_pts_us = pts_us;
    const auto signature = signature_for(result.decision);
    if (signatures_equal(signature, last_presented_signature_)) {
        return result;
    }

    result.should_notify = true;
    return result;
}

void PresentationScheduler::commit_presented(
    const PresentDecision& decision) {
    last_presented_signature_ = signature_for(decision);
}

} // namespace vr
