#include "audio/audio_track_registry.h"

#include "audio/pcm_buffer.h"

#include <utility>

namespace vr {

AudioTrackHandle::AudioTrackHandle(int file_id,
                                   std::shared_ptr<PcmBuffer> buffer,
                                   std::unique_ptr<AudioTrackController> decoder)
    : file_id(file_id)
    , buffer(std::move(buffer))
    , decoder(std::move(decoder)) {}

std::optional<AudioTrackHandle> AudioTrackRegistry::add_or_replace(
    int file_id,
    std::shared_ptr<PcmBuffer> buffer,
    std::unique_ptr<AudioTrackController> decoder) {
    AudioTrackHandle incoming(file_id, std::move(buffer), std::move(decoder));
    auto it = tracks_.find(file_id);
    if (it == tracks_.end()) {
        tracks_.emplace(file_id, std::move(incoming));
        return std::nullopt;
    }

    AudioTrackHandle previous = std::move(it->second);
    it->second = std::move(incoming);
    return previous;
}

std::optional<AudioTrackHandle> AudioTrackRegistry::remove(int file_id) {
    auto it = tracks_.find(file_id);
    if (it == tracks_.end()) {
        return std::nullopt;
    }

    AudioTrackHandle removed = std::move(it->second);
    tracks_.erase(it);
    return removed;
}

std::vector<AudioTrackHandle> AudioTrackRegistry::clear() {
    std::vector<AudioTrackHandle> removed;
    removed.reserve(tracks_.size());
    for (auto& [_, track] : tracks_) {
        removed.push_back(std::move(track));
    }
    tracks_.clear();
    return removed;
}

bool AudioTrackRegistry::set_track_decode_paused(int file_id, bool paused) {
    auto it = tracks_.find(file_id);
    if (it == tracks_.end() || !it->second.decoder) {
        return false;
    }
    it->second.decoder->set_paused(paused);
    return true;
}

void AudioTrackRegistry::set_all_decode_paused(bool paused) {
    for (auto& [_, track] : tracks_) {
        if (track.decoder) {
            track.decoder->set_paused(paused);
        }
    }
}

bool AudioTrackRegistry::notify_seek(int file_id,
                                     int64_t target_pts_us,
                                     SeekType type) {
    auto it = tracks_.find(file_id);
    if (it == tracks_.end() || !it->second.decoder) {
        return false;
    }
    it->second.decoder->notify_seek(target_pts_us, type);
    return true;
}

std::map<int, std::shared_ptr<PcmBuffer>> AudioTrackRegistry::buffers() const {
    std::map<int, std::shared_ptr<PcmBuffer>> result;
    for (const auto& [file_id, track] : tracks_) {
        result[file_id] = track.buffer;
    }
    return result;
}

size_t AudioTrackRegistry::size() const {
    return tracks_.size();
}

bool AudioTrackRegistry::empty() const {
    return tracks_.empty();
}

} // namespace vr
