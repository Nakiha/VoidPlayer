#include "renderer/sync/render_sink.h"
#include <spdlog/spdlog.h>
#include <cmath>
#include <utility>

namespace vr {
namespace {

std::optional<int64_t> frame_end_pts_us(const TrackBuffer& track,
                                        const TextureFrame& frame) {
    const auto next = track.peek(1);
    if (next.has_value() && next->pts_us > frame.pts_us) {
        return next->pts_us;
    }
    if (frame.duration_us > 0) {
        return frame.pts_us + frame.duration_us;
    }
    return std::nullopt;
}

}  // namespace

RenderSink::RenderSink(Clock& clock)
    : clock_(clock)
{}

void RenderSink::set_track(size_t slot,
                           std::shared_ptr<TrackBuffer> track,
                           int file_id,
                           uint64_t track_generation) {
    if (slot < kMaxTracks) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks_[slot] = std::move(track);
        file_ids_[slot] = tracks_[slot] ? file_id : -1;
        track_generations_[slot] = tracks_[slot] ? track_generation : 0;
    }
}

void RenderSink::remove_all_tracks() {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_.fill(nullptr);
    file_ids_.fill(-1);
    track_generations_.fill(0);
}

void RenderSink::set_track_offset(size_t slot, int64_t offset_us) {
    if (slot < kMaxTracks) {
        std::lock_guard<std::mutex> lock(mutex_);
        track_offsets_[slot] = offset_us;
    }
}

PresentDecision RenderSink::evaluate() {
    PresentDecision decision;

    std::array<std::shared_ptr<TrackBuffer>, kMaxTracks> tracks;
    std::array<int64_t, kMaxTracks> track_offsets;
    std::array<int, kMaxTracks> file_ids;
    std::array<uint64_t, kMaxTracks> track_generations;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks = tracks_;
        track_offsets = track_offsets_;
        file_ids = file_ids_;
        track_generations = track_generations_;
    }

    int64_t current_pts_us = clock_.current_pts_us();
    decision.current_pts_us = current_pts_us;

    bool any_active = false;
    bool any_ready = false;
    for (size_t t = 0; t < kMaxTracks; ++t) {
        if (!tracks[t]) {
            decision.frames[t] = std::nullopt;
            decision.file_ids[t] = -1;
            decision.track_generations[t] = 0;
            continue;
        }
        any_active = true;
        decision.file_ids[t] = file_ids[t];
        decision.track_generations[t] = track_generations[t];

        auto& track = tracks[t];
        int64_t effective_pts = current_pts_us - track_offsets[t];

        // 1. Discard expired frames: advance past frames whose display window has passed
        while (true) {
            auto frame = track->peek(0);
            if (!frame.has_value()) {
                break;
            }
            // Prefer the next decoded presentation timestamp over AVFrame
            // duration. Some phone Dolby/HLG HEVC files carry alternating
            // 0.1ms/66ms duration metadata even though their PTS cadence is
            // stable, and trusting that duration makes playback visibly skip.
            const auto end_pts_us = frame_end_pts_us(*track, *frame);
            if (end_pts_us.has_value() && *end_pts_us <= effective_pts) {
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
        const auto end_pts_us = frame_end_pts_us(*track, *frame);
        if (frame->pts_us <= effective_pts &&
            (!end_pts_us.has_value() || effective_pts < *end_pts_us)) {
            // Frame is in its display window - select it
            decision.frames[t] = frame;
            any_ready = true;
        }
        // 4. Check if frame is within tolerance of current time
        else if (std::abs(frame->pts_us - effective_pts) <= kRenderSinkPtsToleranceUs) {
            // Within tolerance - select it
            decision.frames[t] = frame;
            any_ready = true;
        }
        // 5. Frame is in the future (past tolerance)
        else if (frame->pts_us > effective_pts + kRenderSinkPtsToleranceUs) {
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
