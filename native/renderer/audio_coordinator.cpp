#include "renderer/audio_coordinator.h"

#include "audio/audio_output.h"
#include "playback/playback_controller.h"
#include "media/packet_queue.h"

namespace vr {

AudioCoordinator::AudioCoordinator(PlaybackController& playback)
    : playback_(playback) {}

bool AudioCoordinator::available() const {
    return playback_.audio_output() != nullptr;
}

bool AudioCoordinator::register_track(int file_id,
                                      PacketQueue& audio_packet_queue,
                                      const DemuxStats& stats) {
    auto* audio = playback_.audio_output();
    if (stats.audio_stream_index < 0 || !stats.audio_codec_params) {
        return true;
    }
    if (!audio) {
        return false;
    }
    const bool added = audio->add_track(
        file_id,
        audio_packet_queue,
        stats.audio_codec_params,
        stats.audio_time_base);
    return added;
}

void AudioCoordinator::unregister_track(int file_id) {
    auto* audio = playback_.audio_output();
    if (audio && file_id >= 0) {
        audio->remove_track(file_id);
    }
}

void AudioCoordinator::set_active_track(int file_id) {
    auto* audio = playback_.audio_output();
    if (audio) {
        audio->set_active_track(file_id);
    }
}

int AudioCoordinator::active_track() const {
    auto* audio = playback_.audio_output();
    return audio ? audio->active_track() : -1;
}

void AudioCoordinator::set_track_decode_paused(int file_id, bool paused) {
    auto* audio = playback_.audio_output();
    if (audio) {
        audio->set_track_decode_paused(file_id, paused);
    }
}

void AudioCoordinator::set_all_decode_paused(bool paused) {
    auto* audio = playback_.audio_output();
    if (audio) {
        audio->set_all_decode_paused(paused);
    }
}

void AudioCoordinator::notify_seek(int file_id, int64_t target_pts_us, SeekType type) {
    auto* audio = playback_.audio_output();
    if (audio) {
        audio->notify_seek(file_id, target_pts_us, type);
    }
}

AudioOutputStats AudioCoordinator::stats() const {
    auto* audio = playback_.audio_output();
    return audio ? audio->stats() : AudioOutputStats{};
}

} // namespace vr
