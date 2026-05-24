#pragma once

#include "video_renderer/sync/render_sink.h"

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
    bool advance_to_clock(RenderSink& render_sink, int64_t* selected_pts_us = nullptr) const;

private:
    struct PresentedSignature {
        bool should_present = false;
        std::array<bool, kMaxTracks> has_frame{};
        std::array<int64_t, kMaxTracks> pts_us{};
        std::array<int, kMaxTracks> file_ids{};
        std::array<uint64_t, kMaxTracks> track_generations{};
    };

    static PresentedSignature signature_for(const PresentDecision& decision);

    PresentedSignature last_presented_signature_;
};

} // namespace vr
