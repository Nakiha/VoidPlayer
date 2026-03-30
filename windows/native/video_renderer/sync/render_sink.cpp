#include "video_renderer/sync/render_sink.h"
#include <spdlog/spdlog.h>
#include <cmath>

namespace vr {

RenderSink::RenderSink(Clock& clock)
    : clock_(clock)
{}

void RenderSink::add_track(TrackBuffer* track) {
    tracks_.push_back(track);
}

void RenderSink::remove_all_tracks() {
    tracks_.clear();
}

PresentDecision RenderSink::evaluate() {
    PresentDecision decision;

    int64_t current_pts_us = clock_.current_pts_us();
    decision.current_pts_us = current_pts_us;

    // Debug: log first evaluation
    if (!debug_logged_ && !tracks_.empty()) {
        auto peek_frame = tracks_[0]->peek(0);
        if (peek_frame) {
            spdlog::debug("[RenderSink] first eval: clock={:.3f}s frame_pts={:.3f}s dur={:.1f}ms",
                         current_pts_us / 1e6,
                         peek_frame->pts_us / 1e6,
                         peek_frame->duration_us / 1e3);
        } else {
            spdlog::debug("[RenderSink] first eval: clock={:.3f}s NO FRAME", current_pts_us / 1e6);
        }
        debug_logged_ = true;
    }

    if (tracks_.empty()) {
        decision.should_present = false;
        return decision;
    }

    decision.frames.resize(tracks_.size());

    bool all_ready = true;

    for (size_t t = 0; t < tracks_.size(); ++t) {
        TrackBuffer* track = tracks_[t];
        if (!track) {
            decision.frames[t] = std::nullopt;
            all_ready = false;
            continue;
        }

        // 1. Discard expired frames: advance past frames whose display window has passed
        while (true) {
            auto frame = track->peek(0);
            if (!frame.has_value()) {
                break;
            }
            // Frame is expired if its end time has passed
            if (frame->pts_us + frame->duration_us <= current_pts_us) {
                if (!track->advance()) {
                    break; // Cannot advance further
                }
                continue;
            }
            break;
        }

        // 2. Get the current frame after discarding expired ones
        auto frame = track->peek(0);

        if (!frame.has_value()) {
            // No frame available
            decision.frames[t] = std::nullopt;
            all_ready = false;
            continue;
        }

        // 3. Check if frame is in the display window
        if (frame->pts_us <= current_pts_us &&
            current_pts_us < frame->pts_us + frame->duration_us) {
            // Frame is in its display window - select it
            decision.frames[t] = frame;
        }
        // 4. Check if frame is within tolerance of current time
        else if (std::abs(frame->pts_us - current_pts_us) <= PTS_TOLERANCE_US) {
            // Within tolerance - select it
            decision.frames[t] = frame;
        }
        // 5. Frame is in the future (past tolerance)
        else if (frame->pts_us > current_pts_us + PTS_TOLERANCE_US) {
            decision.frames[t] = std::nullopt;
            all_ready = false;
        }
        // 6. Frame is in the past, far beyond tolerance — no valid frame
        else {
            decision.frames[t] = std::nullopt;
            all_ready = false;
        }
    }

    decision.should_present = all_ready;
    return decision;
}

} // namespace vr
