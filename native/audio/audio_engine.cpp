#include "audio/audio_constants.h"
#include "audio/audio_decode_thread.h"
#include "audio/audio_engine.h"
#include "audio/audio_mixer.h"
#include "audio/audio_track_registry.h"
#include "audio/miniaudio_output.h"
#include "audio/pcm_buffer.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace vr {

class AudioEngine::Impl {
public:
    Impl() {
        output_.start();
    }

    ~Impl() {
        clear();
        output_.stop();
    }

    bool add_track(int file_id,
                   PacketQueue& input_queue,
                   const AVCodecParameters* codec_params,
                   AVRational time_base) {
        if (!codec_params) return false;
        auto buffer = std::make_shared<PcmBuffer>(
            kAudioOutputChannels, kAudioOutputSampleRate, kAudioPcmCapacityFrames);
        auto decoder = std::make_unique<AudioDecodeThread>(
            input_queue, *buffer, codec_params, time_base);
        if (!decoder->start()) {
            return false;
        }
        decoder->set_paused(paused_.load());
        std::optional<AudioTrackHandle> previous;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            previous = tracks_.add_or_replace(file_id, buffer, std::move(decoder));
            publish_buffers_locked();
        }
        stop_track(std::move(previous));
        return true;
    }

    void remove_track(int file_id) {
        std::optional<AudioTrackHandle> removed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            removed = tracks_.remove(file_id);
            if (!removed) return;
            publish_buffers_locked();
        }
        stop_track(std::move(removed));
        if (output_.active_track() == file_id) {
            output_.set_active_track(kAudioNoTrack);
        }
    }

    void clear() {
        std::vector<AudioTrackHandle> removed;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            removed = tracks_.clear();
            publish_buffers_locked();
        }
        output_.set_active_track(kAudioNoTrack);
        stop_tracks(std::move(removed));
    }

    void play() {
        paused_.store(false);
        set_all_decode_paused(false);
        output_.set_playing(true);
    }

    void pause() {
        paused_.store(true);
        output_.set_playing(false);
        set_all_decode_paused(true);
    }

    void set_active_track(int file_id) {
        output_.set_active_track(file_id);
    }

    int active_track() const {
        return output_.active_track();
    }

    AudioOutputStats stats() const {
        AudioOutputStats result;
        result.device_initialized = output_.initialized();
        result.playing = !paused_.load();
        result.active_track = output_.active_track();
        result.output_sample_rate = kAudioOutputSampleRate;
        result.output_channels = kAudioOutputChannels;

        std::map<int, std::shared_ptr<PcmBuffer>> buffers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result.registered_track_count = tracks_.size();
            buffers = tracks_.buffers();
        }

        const auto it = buffers.find(result.active_track);
        result.active_track_registered = it != buffers.end() && it->second != nullptr;
        if (result.active_track_registered) {
            const PcmBufferStats pcm = it->second->stats();
            result.active_track_queued_frames = pcm.queued_frames;
            result.active_track_queued_duration_us = pcm.queued_duration_us;
            result.active_track_underrun_frames = pcm.underrun_frames;
            result.active_track_discarded_frames = pcm.discarded_frames;
            result.active_track_seek_trimmed_frames = pcm.seek_trimmed_frames;
        }
        return result;
    }

    void set_track_decode_paused(int file_id, bool paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks_.set_track_decode_paused(file_id, paused);
    }

    void set_all_decode_paused(bool paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks_.set_all_decode_paused(paused);
    }

    void notify_seek(int file_id, int64_t target_pts_us, SeekType type) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks_.notify_seek(file_id, target_pts_us, type);
    }

private:
    void publish_buffers_locked() {
        output_.set_tracks(tracks_.buffers());
    }

    static void stop_track(std::optional<AudioTrackHandle> track) {
        if (!track) return;
        if (track->decoder) track->decoder->stop();
        if (track->buffer) track->buffer->abort();
    }

    static void stop_tracks(std::vector<AudioTrackHandle> tracks) {
        for (auto& track : tracks) {
            if (track.decoder) track.decoder->stop();
            if (track.buffer) track.buffer->abort();
        }
    }

    mutable std::mutex mutex_;
    AudioTrackRegistry tracks_;
    MiniaudioOutput output_;
    std::atomic<bool> paused_{true};
};

AudioEngine::AudioEngine()
    : impl_(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() = default;

bool AudioEngine::add_track(int file_id,
                            PacketQueue& input_queue,
                            const AVCodecParameters* codec_params,
                            AVRational time_base) {
    return impl_->add_track(file_id, input_queue, codec_params, time_base);
}

void AudioEngine::remove_track(int file_id) {
    impl_->remove_track(file_id);
}

void AudioEngine::clear() {
    impl_->clear();
}

void AudioEngine::play() {
    impl_->play();
}

void AudioEngine::pause() {
    impl_->pause();
}

void AudioEngine::set_active_track(int file_id) {
    impl_->set_active_track(file_id);
}

int AudioEngine::active_track() const {
    return impl_->active_track();
}

AudioOutputStats AudioEngine::stats() const {
    return impl_->stats();
}

void AudioEngine::set_track_decode_paused(int file_id, bool paused) {
    impl_->set_track_decode_paused(file_id, paused);
}

void AudioEngine::set_all_decode_paused(bool paused) {
    impl_->set_all_decode_paused(paused);
}

void AudioEngine::notify_seek(int file_id, int64_t target_pts_us, SeekType type) {
    impl_->notify_seek(file_id, target_pts_us, type);
}

} // namespace vr
