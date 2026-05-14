#pragma once

#include "audio/pcm_buffer.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace vr {

constexpr int kAudioNoTrack = -1;

class AudioMixer {
public:
    AudioMixer(int output_channels, size_t fade_frames);

    void set_playing(bool playing);
    void set_active_track(int file_id);
    int active_track() const;
    void set_tracks(const std::map<int, std::shared_ptr<PcmBuffer>>& tracks);
    void render(int16_t* dst, size_t frames);

private:
    std::shared_ptr<PcmBuffer> find_track_locked(int file_id) const;
    void read_track(int file_id, int16_t* dst, size_t frames);
    void discard_unheard(size_t frames, int keep_a, int keep_b);

    const int output_channels_;
    const size_t fade_frames_;

    std::mutex mutex_;
    std::map<int, std::shared_ptr<PcmBuffer>> tracks_;
    std::atomic<bool> playing_{false};
    std::atomic<int> target_track_{kAudioNoTrack};
    std::atomic<int> current_track_{kAudioNoTrack};
    bool fading_ = false;
    int fade_from_ = kAudioNoTrack;
    int fade_to_ = kAudioNoTrack;
    size_t fade_pos_ = 0;
};

} // namespace vr
