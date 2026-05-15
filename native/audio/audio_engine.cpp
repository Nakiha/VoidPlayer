#include "audio/audio_constants.h"
#include "audio/audio_decode_thread.h"
#include "audio/audio_engine.h"
#include "audio/audio_mixer.h"
#include "audio/pcm_buffer.h"
#include "audio/audio_track_registry.h"
#include <spdlog/spdlog.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <mmsystem.h>

namespace vr {
namespace {

class WaveOutOutput {
public:
    ~WaveOutOutput() {
        stop();
    }

    void start() {
        if (running_.load()) return;
        running_.store(true);
        thread_ = std::thread(&WaveOutOutput::run, this);
    }

    void stop() {
        running_.store(false);
        if (thread_.joinable()) {
            thread_.join();
        }
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
    bool open_device() {
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = kAudioOutputChannels;
        fmt.nSamplesPerSec = kAudioOutputSampleRate;
        fmt.wBitsPerSample = kAudioBytesPerSample * 8;
        fmt.nBlockAlign = kAudioOutputChannels * kAudioBytesPerSample;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        MMRESULT mm = waveOutOpen(&wave_out_, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
        if (mm != MMSYSERR_NOERROR) {
            spdlog::warn("[AudioOutput] waveOutOpen failed: {}", static_cast<unsigned>(mm));
            wave_out_ = nullptr;
            return false;
        }
        return true;
    }

    bool check_wave_result(MMRESULT result, const char* operation) const {
        if (result == MMSYSERR_NOERROR) {
            return true;
        }
        spdlog::warn("[AudioOutput] {} failed: {}", operation, static_cast<unsigned>(result));
        return false;
    }

    bool submit_header(WAVEHDR& header) {
        MMRESULT mm = waveOutPrepareHeader(wave_out_, &header, sizeof(WAVEHDR));
        if (!check_wave_result(mm, "waveOutPrepareHeader")) {
            return false;
        }
        mm = waveOutWrite(wave_out_, &header, sizeof(WAVEHDR));
        if (!check_wave_result(mm, "waveOutWrite")) {
            const MMRESULT unprepare = waveOutUnprepareHeader(wave_out_, &header, sizeof(WAVEHDR));
            check_wave_result(unprepare, "waveOutUnprepareHeader after failed write");
            return false;
        }
        return true;
    }

    void unprepare_header(WAVEHDR& header) {
        if ((header.dwFlags & WHDR_PREPARED) == 0) {
            return;
        }
        const MMRESULT mm = waveOutUnprepareHeader(wave_out_, &header, sizeof(WAVEHDR));
        check_wave_result(mm, "waveOutUnprepareHeader");
    }

    void run() {
        if (!open_device()) {
            running_.store(false);
            return;
        }
        const size_t bytes = kAudioOutputBufferFrames * kAudioOutputChannels * sizeof(int16_t);
        std::array<std::vector<int16_t>, kAudioOutputBufferCount> sample_buffers;
        std::array<WAVEHDR, kAudioOutputBufferCount> headers = {};
        for (int i = 0; i < kAudioOutputBufferCount; ++i) {
            sample_buffers[i].resize(kAudioOutputBufferFrames * kAudioOutputChannels);
            mixer_.render(sample_buffers[i].data(), kAudioOutputBufferFrames);
            headers[i].lpData = reinterpret_cast<LPSTR>(sample_buffers[i].data());
            headers[i].dwBufferLength = static_cast<DWORD>(bytes);
            if (!submit_header(headers[i])) {
                running_.store(false);
                break;
            }
        }

        while (running_.load()) {
            bool wrote = false;
            for (int i = 0; i < kAudioOutputBufferCount; ++i) {
                if ((headers[i].dwFlags & WHDR_DONE) == 0) continue;
                unprepare_header(headers[i]);
                std::memset(&headers[i], 0, sizeof(WAVEHDR));
                mixer_.render(sample_buffers[i].data(), kAudioOutputBufferFrames);
                headers[i].lpData = reinterpret_cast<LPSTR>(sample_buffers[i].data());
                headers[i].dwBufferLength = static_cast<DWORD>(bytes);
                if (!submit_header(headers[i])) {
                    running_.store(false);
                    break;
                }
                wrote = true;
            }
            if (!wrote) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        check_wave_result(waveOutReset(wave_out_), "waveOutReset");
        for (auto& header : headers) {
            unprepare_header(header);
        }
        check_wave_result(waveOutClose(wave_out_), "waveOutClose");
        wave_out_ = nullptr;
    }

    AudioMixer mixer_{kAudioOutputChannels, kAudioFadeFrames};
    std::thread thread_;
    std::atomic<bool> running_{false};
    HWAVEOUT wave_out_ = nullptr;
};

} // namespace

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
    WaveOutOutput output_;
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
