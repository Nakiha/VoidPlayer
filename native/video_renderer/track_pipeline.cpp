#include "video_renderer/track_pipeline.h"

#include <spdlog/spdlog.h>

namespace vr {

int TrackPipelineManager::find_empty_slot() const {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int TrackPipelineManager::find_slot_by_file_id(int file_id) const {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i] && tracks_[i]->file_id == file_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void TrackPipelineManager::clear() {
    for (auto& track : tracks_) {
        track.reset();
    }
}

void TrackPipelineManager::stop_all(const TrackCallback& before_stop) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        stop_slot(i, before_stop);
    }
}

void TrackPipelineManager::stop_slot(size_t slot, const TrackCallback& before_stop) {
    if (slot >= kMaxTracks || !tracks_[slot]) {
        return;
    }
    auto& track = tracks_[slot];
    if (before_stop) {
        before_stop(slot, *track);
    }
    if (track->decode_thread) {
        spdlog::info("Renderer: stopping track[{}] decode ({})", slot, track->file_path);
        track->decode_thread->stop();
        spdlog::info("Renderer: track[{}] decode stopped", slot);
    }
    if (track->demux_thread) {
        spdlog::info("Renderer: stopping track[{}] demux ({})", slot, track->file_path);
        track->demux_thread->stop();
        spdlog::info("Renderer: track[{}] demux stopped", slot);
    }
    track.reset();
}

void TrackPipelineManager::compact_from(size_t slot, const MoveCallback& after_move) {
    if (slot >= kMaxTracks) {
        return;
    }
    for (size_t i = slot; i < kMaxTracks - 1; ++i) {
        if (!tracks_[i + 1]) {
            break;
        }
        tracks_[i] = std::move(tracks_[i + 1]);
        if (after_move) {
            after_move(i + 1, i, *tracks_[i]);
        }
    }
}

} // namespace vr
