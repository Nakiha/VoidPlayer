#include "video_renderer/sync/render_sink.h"
#include <spdlog/spdlog.h>
#include <cmath>

namespace vr {

RenderSink::RenderSink(Clock& clock)
    : clock_(clock)
{}

void RenderSink::set_track(size_t slot, TrackBuffer* track) {
    if (slot < kMaxTracks) {
        tracks_[slot] = track;
    }
}

void RenderSink::remove_all_tracks() {
    tracks_.fill(nullptr);
}

void RenderSink::set_track_offset(size_t slot, int64_t offset_us) {
    if (slot < kMaxTracks) {
        track_offsets_[slot] = offset_us;
    }
}

PresentDecision RenderSink::evaluate() {
    PresentDecision decision;

    int64_t current_pts_us = clock_.current_pts_us();
    decision.current_pts_us = current_pts_us;

    bool any_active = false;
    bool any_ready = false;
    for (size_t t = 0; t < kMaxTracks; ++t) {
        if (!tracks_[t]) {
            decision.frames[t] = std::nullopt;
            continue;
        }
        any_active = true;

        TrackBuffer* track = tracks_[t];
        int64_t effective_pts = current_pts_us - track_offsets_[t];

        // 1. Discard expired frames: advance past frames whose display window has passed
        while (true) {
            auto frame = track->peek(0);
            if (!frame.has_value()) {
                break;
            }
            // Zero-duration frames: advance only when the next frame's PTS has arrived
            if (frame->duration_us == 0) {
                auto next = track->peek(1);
                if (next.has_value() && next->pts_us <= effective_pts) {
                    if (!track->advance()) break;
                    continue;
                }
                break;
            }
            // Frame is expired if its end time has passed
            if (frame->pts_us + frame->duration_us <= effective_pts) {
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
            continue;
        }

        // 3. Check if frame is in the display window
        if (frame->pts_us <= effective_pts &&
            (frame->duration_us == 0 || effective_pts < frame->pts_us + frame->duration_us)) {
            // Frame is in its display window - select it
            decision.frames[t] = frame;
            any_ready = true;
        }
        // 4. Check if frame is within tolerance of current time
        else if (std::abs(frame->pts_us - effective_pts) <= PTS_TOLERANCE_US) {
            // Within tolerance - select it
            decision.frames[t] = frame;
            any_ready = true;
        }
        // 5. Frame is in the future (past tolerance)
        else if (frame->pts_us > effective_pts + PTS_TOLERANCE_US) {
            decision.frames[t] = std::nullopt;
        }
        // 6. Frame is in the past, far beyond tolerance — no valid frame
        else {
            decision.frames[t] = std::nullopt;
        }
    }

    decision.should_present = any_active && any_ready;
    return decision;
}

} // namespace vr
