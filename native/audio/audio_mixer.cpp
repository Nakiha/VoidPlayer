#include "audio/audio_mixer.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace vr {

AudioMixer::AudioMixer(int output_channels, size_t fade_frames)
    : output_channels_(std::max(1, output_channels))
    , fade_frames_(std::max<size_t>(1, fade_frames)) {}

void AudioMixer::set_playing(bool playing) {
    playing_.store(playing);
}

void AudioMixer::set_active_track(int file_id) {
    target_track_.store(file_id);
}

int AudioMixer::active_track() const {
    return target_track_.load();
}

void AudioMixer::set_tracks(const std::map<int, std::shared_ptr<PcmBuffer>>& tracks) {
    std::lock_guard<std::mutex> lock(mutex_);
    tracks_ = tracks;
}

std::shared_ptr<PcmBuffer> AudioMixer::find_track_locked(int file_id) const {
    auto it = tracks_.find(file_id);
    return it == tracks_.end() ? nullptr : it->second;
}

void AudioMixer::read_track(int file_id, int16_t* dst, size_t frames) {
    std::shared_ptr<PcmBuffer> buffer;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer = find_track_locked(file_id);
    }
    if (buffer) {
        buffer->pop(dst, frames);
    } else {
        std::memset(dst, 0, frames * static_cast<size_t>(output_channels_) * sizeof(int16_t));
    }
}

void AudioMixer::discard_unheard(size_t frames, int keep_a, int keep_b) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [file_id, buffer] : tracks_) {
        if (file_id == keep_a || file_id == keep_b || !buffer) continue;
        buffer->discard(frames);
    }
}

void AudioMixer::render(int16_t* dst, size_t frames) {
    if (!dst || frames == 0) {
        return;
    }

    const size_t sample_count = frames * static_cast<size_t>(output_channels_);
    if (!playing_.load()) {
        std::memset(dst, 0, sample_count * sizeof(int16_t));
        return;
    }

    const int target = target_track_.load();
    if (target != current_track_.load() && !fading_) {
        fade_from_ = current_track_.load();
        fade_to_ = target;
        fade_pos_ = 0;
        fading_ = true;
    }

    if (!fading_) {
        read_track(target, dst, frames);
        current_track_.store(target);
        discard_unheard(frames, target, kAudioNoTrack);
        return;
    }

    std::vector<int16_t> from(sample_count);
    std::vector<int16_t> to(sample_count);
    read_track(fade_from_, from.data(), frames);
    read_track(fade_to_, to.data(), frames);
    for (size_t f = 0; f < frames; ++f) {
        const float t = static_cast<float>(std::min(fade_pos_, fade_frames_)) /
            static_cast<float>(fade_frames_);
        for (int c = 0; c < output_channels_; ++c) {
            const size_t idx = f * static_cast<size_t>(output_channels_) + static_cast<size_t>(c);
            const float mixed = static_cast<float>(from[idx]) * (1.0f - t) +
                static_cast<float>(to[idx]) * t;
            dst[idx] = static_cast<int16_t>(std::clamp(mixed, -32768.0f, 32767.0f));
        }
        if (fade_pos_ < fade_frames_) ++fade_pos_;
    }
    if (fade_pos_ >= fade_frames_) {
        current_track_.store(fade_to_);
        fading_ = false;
    }
    discard_unheard(frames, fade_from_, fade_to_);
}

} // namespace vr
