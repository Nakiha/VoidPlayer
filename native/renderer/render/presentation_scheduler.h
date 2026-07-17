#pragma once

#include "renderer/sync/render_sink.h"

#include <array>
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
    void commit_presented(const PresentDecision& decision);

private:
    struct PresentedFrameSignature {
        bool present = false;
        int64_t pts_us = kNoTimestampUs;
        int64_t dts_us = kNoTimestampUs;
        int32_t source_packet_index = kInvalidSourcePacketIndex;
        uintptr_t storage_identity = 0;
        int file_id = -1;
        uint64_t track_generation = 0;
    };
    struct PresentedSignature {
        bool should_present = false;
        std::array<PresentedFrameSignature, kMaxTracks> frames;
    };

    static PresentedSignature signature_for(const PresentDecision& decision);
    static bool signatures_equal(const PresentedSignature& left,
                                 const PresentedSignature& right);

    PresentedSignature last_presented_signature_;
};

} // namespace vr
