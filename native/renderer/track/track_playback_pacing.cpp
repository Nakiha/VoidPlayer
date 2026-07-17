#include "renderer/track/track_playback_pacing.h"

#include <algorithm>
#include <limits>

namespace vr {
namespace {

constexpr int64_t kDefaultFrameIntervalUs = 33333;
constexpr int64_t kMinimumHighWatermarkUs = 2000;
constexpr int64_t kMaximumHighWatermarkUs = 250000;

bool track_packet_eof(const TrackPipeline& track) {
    return track.packet_queue &&
           track.packet_queue->is_eof() &&
           track.packet_queue->empty();
}

int64_t frame_interval_us(const TextureFrame& current,
                          const std::optional<TextureFrame>& next) {
    if (next.has_value() && next->pts_us > current.pts_us) {
        return next->pts_us - current.pts_us;
    }
    if (current.duration_us > 0) {
        return current.duration_us;
    }
    return kDefaultFrameIntervalUs;
}

} // namespace

PlaybackPacingSnapshot snapshot_track_playback_pacing(
    const TrackPipelineManager& tracks,
    int64_t current_pts_us) {
    PlaybackPacingSnapshot snapshot;
    snapshot.resume_ready = true;
    snapshot.all_active_tracks_eof = true;
    snapshot.min_buffered_frames = std::numeric_limits<size_t>::max();

    double bottleneck_fill = std::numeric_limits<double>::infinity();
    int64_t minimum_frontier = std::numeric_limits<int64_t>::max();

    for (size_t slot = 0; slot < kMaxTracks; ++slot) {
        const auto& track = tracks[slot];
        if (!track) {
            continue;
        }

        snapshot.has_active_tracks = true;
        const bool packet_eof = track_packet_eof(*track);
        snapshot.all_active_tracks_eof =
            snapshot.all_active_tracks_eof && packet_eof;

        if (!track->track_buffer) {
            snapshot.preroll_blocked = true;
            snapshot.starvation_risk = !packet_eof;
            snapshot.resume_ready = packet_eof;
            snapshot.min_buffered_frames = 0;
            if (!packet_eof) {
                snapshot.frontier_limited = true;
                snapshot.bottleneck_slot = static_cast<int>(slot);
                snapshot.bottleneck_buffered_frames = 0;
                snapshot.bottleneck_target_frames = 1;
                snapshot.safe_frontier_us = current_pts_us;
                snapshot.headroom_us = 0;
                snapshot.high_watermark_us = kMinimumHighWatermarkUs;
            }
            continue;
        }

        const TrackState state = track->track_buffer->state();
        if (state == TrackState::Empty ||
            state == TrackState::Buffering ||
            state == TrackState::Flushing) {
            snapshot.preroll_blocked = true;
        }
        if (state == TrackState::Error) {
            continue;
        }

        const size_t buffered_frames = track->track_buffer->total_count();
        snapshot.min_buffered_frames =
            std::min(snapshot.min_buffered_frames, buffered_frames);

        const auto current = track->track_buffer->peek(0);
        if (!current.has_value()) {
            if (!packet_eof) {
                snapshot.starvation_risk = true;
                snapshot.resume_ready = false;
                snapshot.frontier_limited = true;
                minimum_frontier =
                    std::min(minimum_frontier, current_pts_us);
                if (bottleneck_fill > 0.0) {
                    bottleneck_fill = 0.0;
                    snapshot.bottleneck_slot = static_cast<int>(slot);
                    snapshot.bottleneck_buffered_frames = 0;
                    snapshot.bottleneck_target_frames = 1;
                    snapshot.headroom_us = 0;
                    snapshot.high_watermark_us =
                        kMinimumHighWatermarkUs;
                }
            }
            continue;
        }

        const int64_t first_global_pts =
            current->pts_us + track->offset_us;
        if (current_pts_us + kRenderSinkPtsToleranceUs < first_global_pts) {
            // A positive-offset track has not entered the global timeline yet.
            continue;
        }
        if (packet_eof) {
            // All remaining frames are already available, so this track cannot
            // cause a decode underrun. Exact presentation still consumes them
            // in order.
            continue;
        }

        const auto next = track->track_buffer->peek(1);
        const int64_t interval_us = frame_interval_us(*current, next);
        const size_t maximum_ahead_frames =
            track->track_buffer->max_count() > 0
                ? track->track_buffer->max_count() - 1
                : 0;
        const size_t target_ahead_frames =
            std::clamp<size_t>(maximum_ahead_frames, 1, 2);
        const size_t target_buffered_frames = std::max<size_t>(
            1,
            std::min(
                track->track_buffer->max_count(),
                target_ahead_frames + 1));
        const int64_t target_intervals =
            static_cast<int64_t>(target_ahead_frames);
        const int64_t high_watermark_us = std::clamp<int64_t>(
            interval_us * target_intervals,
            kMinimumHighWatermarkUs,
            kMaximumHighWatermarkUs);

        if (!next.has_value()) {
            // The head frame has not been presented yet. Admit it before
            // declaring an underrun; otherwise a one-frame preroll cannot
            // release its slot for the decoder and pacing deadlocks. Once
            // the renderer commits this frame, an actually empty buffer will
            // enter the normal rebuffering path on the next evaluation.
            continue;
        }

        const auto tail = track->track_buffer->peek(
            static_cast<int>(buffered_frames - 1));
        const int64_t frontier_us =
            (tail.has_value() ? tail->pts_us : next->pts_us) +
            track->offset_us;
        const int64_t headroom_us =
            std::max<int64_t>(0, frontier_us - current_pts_us);
        const size_t buffered_ahead_frames =
            buffered_frames > 0 ? buffered_frames - 1 : 0;
        const double fill = target_ahead_frames > 0
            ? static_cast<double>(std::min(
                  buffered_ahead_frames, target_ahead_frames)) /
                  static_cast<double>(target_ahead_frames)
            : 1.0;

        snapshot.frontier_limited = true;
        minimum_frontier = std::min(minimum_frontier, frontier_us);
        if (fill < bottleneck_fill) {
            bottleneck_fill = fill;
            snapshot.bottleneck_slot = static_cast<int>(slot);
            snapshot.bottleneck_buffered_frames = buffered_frames;
            snapshot.bottleneck_target_frames = target_buffered_frames;
            snapshot.headroom_us = headroom_us;
            snapshot.high_watermark_us = high_watermark_us;
        }
        if (buffered_frames < target_buffered_frames) {
            snapshot.resume_ready = false;
        }
    }

    if (!snapshot.has_active_tracks) {
        snapshot.resume_ready = false;
        snapshot.all_active_tracks_eof = false;
        snapshot.min_buffered_frames = 0;
    } else if (snapshot.min_buffered_frames ==
               std::numeric_limits<size_t>::max()) {
        snapshot.min_buffered_frames = 0;
    }

    if (snapshot.frontier_limited) {
        snapshot.safe_frontier_us =
            minimum_frontier == std::numeric_limits<int64_t>::max()
                ? current_pts_us
                : minimum_frontier;
    }
    if (snapshot.all_active_tracks_eof) {
        snapshot.resume_ready = true;
        snapshot.starvation_risk = false;
        snapshot.frontier_limited = false;
        snapshot.bottleneck_slot = -1;
        snapshot.bottleneck_buffered_frames = 0;
        snapshot.bottleneck_target_frames = 0;
        snapshot.safe_frontier_us = 0;
        snapshot.headroom_us = 0;
        snapshot.high_watermark_us = 0;
    }
    return snapshot;
}

} // namespace vr
