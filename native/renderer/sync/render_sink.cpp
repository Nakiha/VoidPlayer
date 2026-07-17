#include "renderer/sync/render_sink.h"
#include <spdlog/spdlog.h>
#include <utility>

namespace vr {
namespace {

bool same_decoded_frame(const TextureFrame& left,
                        const TextureFrame& right) {
    if (left.source_packet_index != kInvalidSourcePacketIndex &&
        right.source_packet_index != kInvalidSourcePacketIndex) {
        return left.source_packet_index == right.source_packet_index;
    }
    if (left.texture_handle || right.texture_handle) {
        return left.texture_handle == right.texture_handle &&
               left.pts_us == right.pts_us &&
               left.dts_us == right.dts_us;
    }
    if (left.cpu_data || right.cpu_data) {
        return left.cpu_data == right.cpu_data &&
               left.pts_us == right.pts_us &&
               left.dts_us == right.dts_us;
    }
    return left.pts_us == right.pts_us &&
           left.dts_us == right.dts_us;
}

} // namespace

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
    std::array<std::optional<TextureFrame>, kMaxTracks> queue_heads;
    std::array<int64_t, kMaxTracks> global_head_pts{};
    std::optional<int64_t> next_global_event_pts;

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
        queue_heads[t] = track->peek(0);
        if (!queue_heads[t].has_value()) {
            continue;
        }
        global_head_pts[t] =
            queue_heads[t]->pts_us + track_offsets[t];
        if (global_head_pts[t] <=
            current_pts_us + kRenderSinkPtsToleranceUs) {
            next_global_event_pts = std::min(
                next_global_event_pts.value_or(global_head_pts[t]),
                global_head_pts[t]);
        }
    }

    bool any_ready = false;
    if (next_global_event_pts.has_value()) {
        const int64_t coalesce_limit_us =
            *next_global_event_pts + kRenderSinkPtsToleranceUs;
        for (size_t t = 0; t < kMaxTracks; ++t) {
            if (!queue_heads[t].has_value() ||
                global_head_pts[t] > coalesce_limit_us) {
                continue;
            }
            decision.frames[t] = queue_heads[t];
            any_ready = true;
        }
    }

    decision.should_present = any_active && any_ready;
    return decision;
}

size_t RenderSink::commit_presented(const PresentDecision& decision) {
    std::array<std::shared_ptr<TrackBuffer>, kMaxTracks> tracks;
    std::array<int, kMaxTracks> file_ids;
    std::array<uint64_t, kMaxTracks> track_generations;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks = tracks_;
        file_ids = file_ids_;
        track_generations = track_generations_;
    }

    size_t committed = 0;
    for (size_t slot = 0; slot < kMaxTracks; ++slot) {
        if (!tracks[slot] ||
            !decision.frames[slot].has_value() ||
            decision.file_ids[slot] != file_ids[slot] ||
            decision.track_generations[slot] !=
                track_generations[slot]) {
            continue;
        }
        const auto front = tracks[slot]->peek(0);
        if (!front.has_value() ||
            !same_decoded_frame(*front, *decision.frames[slot])) {
            continue;
        }
        if (tracks[slot]->advance()) {
            ++committed;
        }
    }
    return committed;
}

} // namespace vr
