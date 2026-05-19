#include "audio/miniaudio_output.h"

#include "audio/audio_constants.h"
#include "audio/audio_mixer.h"
#include "audio/pcm_buffer.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <mutex>

#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#if defined(_WIN32)
#define MA_ENABLE_WASAPI
#elif defined(__APPLE__)
#define MA_ENABLE_COREAUDIO
#endif
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_VORBIS
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace vr {

class MiniaudioOutput::Impl {
public:
    Impl()
        : mixer_(kAudioOutputChannels, kAudioFadeFrames) {}

    ~Impl() {
        stop();
    }

    void start() {
        std::lock_guard<std::mutex> lock(device_mutex_);
        if (initialized_) {
            return;
        }

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_s16;
        config.playback.channels = kAudioOutputChannels;
        config.sampleRate = kAudioOutputSampleRate;
        config.periodSizeInFrames = kAudioOutputBufferFrames;
        config.periods = kAudioOutputBufferCount;
        config.performanceProfile = ma_performance_profile_low_latency;
        config.noPreSilencedOutputBuffer = MA_TRUE;
        config.dataCallback = &MiniaudioOutput::Impl::data_callback;
        config.pUserData = this;

        const ma_result init_result = ma_device_init(nullptr, &config, &device_);
        if (init_result != MA_SUCCESS) {
            spdlog::warn("[AudioOutput] ma_device_init failed: {}",
                         ma_result_description(init_result));
            return;
        }
        initialized_ = true;

        const ma_result start_result = ma_device_start(&device_);
        if (start_result != MA_SUCCESS) {
            spdlog::warn("[AudioOutput] ma_device_start failed: {}",
                         ma_result_description(start_result));
            ma_device_uninit(&device_);
            initialized_ = false;
        }
    }

    void stop() {
        std::lock_guard<std::mutex> lock(device_mutex_);
        if (!initialized_) {
            return;
        }

        const ma_result stop_result = ma_device_stop(&device_);
        if (stop_result != MA_SUCCESS) {
            spdlog::warn("[AudioOutput] ma_device_stop failed: {}",
                         ma_result_description(stop_result));
        }
        ma_device_uninit(&device_);
        initialized_ = false;
    }

    void set_playing(bool playing) {
        mixer_.set_playing(playing);
    }

    void set_active_track(int file_id) {
        mixer_.set_active_track(file_id);
    }

    int active_track() const {
        return mixer_.active_track();
    }

    void set_tracks(const std::map<int, std::shared_ptr<PcmBuffer>>& tracks) {
        mixer_.set_tracks(tracks);
    }

private:
    static void data_callback(ma_device* device,
                              void* output,
                              const void* input,
                              ma_uint32 frame_count) {
        (void)input;
        if (!device || !output || frame_count == 0) {
            return;
        }
        auto* self = static_cast<MiniaudioOutput::Impl*>(device->pUserData);
        if (!self) {
            return;
        }
        self->mixer_.render(static_cast<int16_t*>(output),
                            static_cast<size_t>(frame_count));
    }

    AudioMixer mixer_;
    std::mutex device_mutex_;
    ma_device device_{};
    bool initialized_ = false;
};

MiniaudioOutput::MiniaudioOutput()
    : impl_(std::make_unique<Impl>()) {}

MiniaudioOutput::~MiniaudioOutput() = default;

void MiniaudioOutput::start() {
    impl_->start();
}

void MiniaudioOutput::stop() {
    impl_->stop();
}

void MiniaudioOutput::set_playing(bool playing) {
    impl_->set_playing(playing);
}

void MiniaudioOutput::set_active_track(int file_id) {
    impl_->set_active_track(file_id);
}

int MiniaudioOutput::active_track() const {
    return impl_->active_track();
}

void MiniaudioOutput::set_tracks(
    const std::map<int, std::shared_ptr<PcmBuffer>>& tracks) {
    impl_->set_tracks(tracks);
}

} // namespace vr
