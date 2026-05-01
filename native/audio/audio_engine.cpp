#include "audio/audio_engine.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <mmsystem.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace vr {
namespace {

constexpr int kOutputSampleRate = 48000;
constexpr int kOutputChannels = 2;
constexpr int kBytesPerSample = 2;
constexpr int kOutputBufferFrames = 480;  // 10ms
constexpr int kOutputBufferCount = 4;
constexpr size_t kPcmCapacityFrames = kOutputSampleRate / 2;  // 500ms
constexpr size_t kFadeFrames = 480;  // 10ms
constexpr int kNoTrack = -1;

class PcmBuffer {
public:
    void push(const int16_t* samples, size_t frames) {
        if (!samples || frames == 0) return;
        std::unique_lock<std::mutex> lock(mutex_);
        const size_t sample_count = frames * kOutputChannels;
        for (size_t i = 0; i < sample_count; ++i) {
            while (!aborted_ && samples_.size() >= kPcmCapacityFrames * kOutputChannels) {
                not_full_.wait_for(lock, std::chrono::milliseconds(5));
            }
            if (aborted_) return;
            samples_.push_back(samples[i]);
        }
        not_empty_.notify_all();
    }

    void pop(int16_t* dst, size_t frames) {
        if (!dst) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t sample_count = frames * kOutputChannels;
        for (size_t i = 0; i < sample_count; ++i) {
            if (samples_.empty()) {
                dst[i] = 0;
            } else {
                dst[i] = samples_.front();
                samples_.pop_front();
            }
        }
        not_full_.notify_all();
    }

    void discard(size_t frames) {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t sample_count = std::min(samples_.size(), frames * kOutputChannels);
        for (size_t i = 0; i < sample_count; ++i) {
            samples_.pop_front();
        }
        not_full_.notify_all();
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.clear();
        not_full_.notify_all();
    }

    void abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
        samples_.clear();
        not_full_.notify_all();
        not_empty_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<int16_t> samples_;
    bool aborted_ = false;
};

class AudioDecodeThread {
public:
    AudioDecodeThread(PacketQueue& input_queue,
                      PcmBuffer& output_buffer,
                      const AVCodecParameters* codec_params,
                      AVRational time_base)
        : input_queue_(input_queue)
        , output_buffer_(output_buffer)
        , codec_params_(codec_params)
        , time_base_(time_base) {}

    ~AudioDecodeThread() {
        stop();
    }

    bool start() {
        if (running_.load()) return false;
        codec_ = avcodec_find_decoder(codec_params_->codec_id);
        if (!codec_) {
            spdlog::warn("[AudioDecodeThread] No decoder for codec_id={}",
                         static_cast<int>(codec_params_->codec_id));
            return false;
        }
        codec_ctx_ = avcodec_alloc_context3(codec_);
        if (!codec_ctx_) return false;
        if (avcodec_parameters_to_context(codec_ctx_, codec_params_) < 0) {
            spdlog::warn("[AudioDecodeThread] avcodec_parameters_to_context failed");
            avcodec_free_context(&codec_ctx_);
            return false;
        }
        if (codec_ctx_->ch_layout.nb_channels <= 0) {
            av_channel_layout_default(&codec_ctx_->ch_layout, codec_params_->ch_layout.nb_channels > 0
                ? codec_params_->ch_layout.nb_channels
                : 2);
        }
        if (avcodec_open2(codec_ctx_, codec_, nullptr) < 0) {
            spdlog::warn("[AudioDecodeThread] avcodec_open2 failed");
            avcodec_free_context(&codec_ctx_);
            return false;
        }
        if (!init_resampler()) {
            avcodec_free_context(&codec_ctx_);
            return false;
        }
        running_.store(true);
        thread_ = std::thread(&AudioDecodeThread::run, this);
        return true;
    }

    void stop() {
        running_.store(false);
        input_queue_.abort();
        output_buffer_.abort();
        if (thread_.joinable()) {
            thread_.join();
        }
        if (swr_) {
            swr_free(&swr_);
        }
        if (codec_ctx_) {
            avcodec_free_context(&codec_ctx_);
        }
    }

    void set_paused(bool paused) {
        decode_paused_.store(paused);
    }

    void notify_seek(int64_t, SeekType) {
        seek_pending_.store(true);
        output_buffer_.flush();
    }

private:
    bool init_resampler() {
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, kOutputChannels);
        int ret = swr_alloc_set_opts2(
            &swr_,
            &out_layout,
            AV_SAMPLE_FMT_S16,
            kOutputSampleRate,
            &codec_ctx_->ch_layout,
            codec_ctx_->sample_fmt,
            codec_ctx_->sample_rate,
            0,
            nullptr);
        av_channel_layout_uninit(&out_layout);
        if (ret < 0 || !swr_) {
            spdlog::warn("[AudioDecodeThread] swr_alloc_set_opts2 failed");
            return false;
        }
        if (swr_init(swr_) < 0) {
            spdlog::warn("[AudioDecodeThread] swr_init failed");
            return false;
        }
        return true;
    }

    void flush_after_seek_if_needed() {
        if (!seek_pending_.exchange(false)) return;
        avcodec_flush_buffers(codec_ctx_);
        if (swr_) swr_close(swr_);
        if (swr_) swr_init(swr_);
        output_buffer_.flush();
    }

    void receive_frames(AVFrame* frame) {
        while (running_.load()) {
            int ret = avcodec_receive_frame(codec_ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                return;
            }
            if (ret < 0) {
                spdlog::warn("[AudioDecodeThread] receive_frame failed: {:#x}",
                             static_cast<unsigned>(ret));
                return;
            }

            const int out_capacity = static_cast<int>(
                av_rescale_rnd(
                    swr_get_delay(swr_, codec_ctx_->sample_rate) + frame->nb_samples,
                    kOutputSampleRate,
                    codec_ctx_->sample_rate,
                    AV_ROUND_UP));
            std::vector<int16_t> pcm(static_cast<size_t>(out_capacity) * kOutputChannels);
            uint8_t* out_data[] = {reinterpret_cast<uint8_t*>(pcm.data())};
            int out_samples = swr_convert(
                swr_,
                out_data,
                out_capacity,
                const_cast<const uint8_t**>(frame->extended_data),
                frame->nb_samples);
            if (out_samples > 0) {
                output_buffer_.push(pcm.data(), static_cast<size_t>(out_samples));
            }
            av_frame_unref(frame);
        }
    }

    void run() {
        AVFrame* frame = av_frame_alloc();
        if (!frame) return;
        while (running_.load()) {
            if (decode_paused_.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            flush_after_seek_if_needed();
            AVPacket* pkt = input_queue_.pop();
            if (!pkt) {
                if (!running_.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            flush_after_seek_if_needed();
            int ret = avcodec_send_packet(codec_ctx_, pkt);
            av_packet_free(&pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                spdlog::warn("[AudioDecodeThread] send_packet failed: {:#x}",
                             static_cast<unsigned>(ret));
                continue;
            }
            receive_frames(frame);
        }
        av_frame_free(&frame);
    }

    PacketQueue& input_queue_;
    PcmBuffer& output_buffer_;
    const AVCodecParameters* codec_params_;
    AVRational time_base_;
    const AVCodec* codec_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    SwrContext* swr_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> decode_paused_{true};
    std::atomic<bool> seek_pending_{false};
};

struct AudioTrack {
    int file_id = 0;
    std::shared_ptr<PcmBuffer> buffer;
    std::unique_ptr<AudioDecodeThread> decoder;
};

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
        playing_.store(playing);
    }

    void set_active_track(int file_id) {
        target_track_.store(file_id);
    }

    int active_track() const {
        return target_track_.load();
    }

    void set_tracks(const std::map<int, std::shared_ptr<PcmBuffer>>& tracks) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracks_ = tracks;
    }

private:
    std::shared_ptr<PcmBuffer> find_track_locked(int file_id) const {
        auto it = tracks_.find(file_id);
        return it == tracks_.end() ? nullptr : it->second;
    }

    void read_track(int file_id, int16_t* dst, size_t frames) {
        std::shared_ptr<PcmBuffer> buffer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            buffer = find_track_locked(file_id);
        }
        if (buffer) {
            buffer->pop(dst, frames);
        } else {
            std::memset(dst, 0, frames * kOutputChannels * sizeof(int16_t));
        }
    }

    void discard_unheard(size_t frames, int keep_a, int keep_b) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [file_id, buffer] : tracks_) {
            if (file_id == keep_a || file_id == keep_b || !buffer) continue;
            buffer->discard(frames);
        }
    }

    void render(int16_t* dst, size_t frames) {
        if (!playing_.load()) {
            std::memset(dst, 0, frames * kOutputChannels * sizeof(int16_t));
            discard_unheard(frames, kNoTrack, kNoTrack);
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
            discard_unheard(frames, target, kNoTrack);
            return;
        }

        std::vector<int16_t> from(frames * kOutputChannels);
        std::vector<int16_t> to(frames * kOutputChannels);
        read_track(fade_from_, from.data(), frames);
        read_track(fade_to_, to.data(), frames);
        for (size_t f = 0; f < frames; ++f) {
            const float t = static_cast<float>(std::min(fade_pos_, kFadeFrames)) /
                static_cast<float>(kFadeFrames);
            for (int c = 0; c < kOutputChannels; ++c) {
                const size_t idx = f * kOutputChannels + c;
                const float mixed = static_cast<float>(from[idx]) * (1.0f - t) +
                    static_cast<float>(to[idx]) * t;
                dst[idx] = static_cast<int16_t>(std::clamp(mixed, -32768.0f, 32767.0f));
            }
            if (fade_pos_ < kFadeFrames) ++fade_pos_;
        }
        if (fade_pos_ >= kFadeFrames) {
            current_track_.store(fade_to_);
            fading_ = false;
        }
        discard_unheard(frames, fade_from_, fade_to_);
    }

    bool open_device() {
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = kOutputChannels;
        fmt.nSamplesPerSec = kOutputSampleRate;
        fmt.wBitsPerSample = kBytesPerSample * 8;
        fmt.nBlockAlign = kOutputChannels * kBytesPerSample;
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        MMRESULT mm = waveOutOpen(&wave_out_, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
        if (mm != MMSYSERR_NOERROR) {
            spdlog::warn("[AudioOutput] waveOutOpen failed: {}", static_cast<unsigned>(mm));
            wave_out_ = nullptr;
            return false;
        }
        return true;
    }

    void run() {
        if (!open_device()) return;
        const size_t bytes = kOutputBufferFrames * kOutputChannels * sizeof(int16_t);
        std::array<std::vector<int16_t>, kOutputBufferCount> sample_buffers;
        std::array<WAVEHDR, kOutputBufferCount> headers = {};
        for (int i = 0; i < kOutputBufferCount; ++i) {
            sample_buffers[i].resize(kOutputBufferFrames * kOutputChannels);
            render(sample_buffers[i].data(), kOutputBufferFrames);
            headers[i].lpData = reinterpret_cast<LPSTR>(sample_buffers[i].data());
            headers[i].dwBufferLength = static_cast<DWORD>(bytes);
            waveOutPrepareHeader(wave_out_, &headers[i], sizeof(WAVEHDR));
            waveOutWrite(wave_out_, &headers[i], sizeof(WAVEHDR));
        }

        while (running_.load()) {
            bool wrote = false;
            for (int i = 0; i < kOutputBufferCount; ++i) {
                if ((headers[i].dwFlags & WHDR_DONE) == 0) continue;
                waveOutUnprepareHeader(wave_out_, &headers[i], sizeof(WAVEHDR));
                std::memset(&headers[i], 0, sizeof(WAVEHDR));
                render(sample_buffers[i].data(), kOutputBufferFrames);
                headers[i].lpData = reinterpret_cast<LPSTR>(sample_buffers[i].data());
                headers[i].dwBufferLength = static_cast<DWORD>(bytes);
                waveOutPrepareHeader(wave_out_, &headers[i], sizeof(WAVEHDR));
                waveOutWrite(wave_out_, &headers[i], sizeof(WAVEHDR));
                wrote = true;
            }
            if (!wrote) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        waveOutReset(wave_out_);
        for (auto& header : headers) {
            if (header.dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(wave_out_, &header, sizeof(WAVEHDR));
            }
        }
        waveOutClose(wave_out_);
        wave_out_ = nullptr;
    }

    std::mutex mutex_;
    std::map<int, std::shared_ptr<PcmBuffer>> tracks_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::atomic<int> target_track_{kNoTrack};
    std::atomic<int> current_track_{kNoTrack};
    bool fading_ = false;
    int fade_from_ = kNoTrack;
    int fade_to_ = kNoTrack;
    size_t fade_pos_ = 0;
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
        auto buffer = std::make_shared<PcmBuffer>();
        auto decoder = std::make_unique<AudioDecodeThread>(
            input_queue, *buffer, codec_params, time_base);
        if (!decoder->start()) {
            return false;
        }
        decoder->set_paused(paused_.load());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tracks_[file_id] = AudioTrack{file_id, buffer, std::move(decoder)};
            publish_buffers_locked();
        }
        return true;
    }

    void remove_track(int file_id) {
        std::unique_ptr<AudioDecodeThread> decoder;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = tracks_.find(file_id);
            if (it == tracks_.end()) return;
            decoder = std::move(it->second.decoder);
            tracks_.erase(it);
            publish_buffers_locked();
        }
        if (decoder) decoder->stop();
        if (output_.active_track() == file_id) {
            output_.set_active_track(kNoTrack);
        }
    }

    void clear() {
        std::map<int, AudioTrack> old;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            old.swap(tracks_);
            publish_buffers_locked();
        }
        output_.set_active_track(kNoTrack);
        for (auto& [_, track] : old) {
            if (track.decoder) track.decoder->stop();
            if (track.buffer) track.buffer->abort();
        }
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
        auto it = tracks_.find(file_id);
        if (it != tracks_.end() && it->second.decoder) {
            it->second.decoder->set_paused(paused);
        }
    }

    void set_all_decode_paused(bool paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [_, track] : tracks_) {
            if (track.decoder) track.decoder->set_paused(paused);
        }
    }

    void notify_seek(int file_id, int64_t target_pts_us, SeekType type) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = tracks_.find(file_id);
        if (it != tracks_.end() && it->second.decoder) {
            it->second.decoder->notify_seek(target_pts_us, type);
        }
    }

private:
    void publish_buffers_locked() {
        std::map<int, std::shared_ptr<PcmBuffer>> buffers;
        for (auto& [file_id, track] : tracks_) {
            buffers[file_id] = track.buffer;
        }
        output_.set_tracks(buffers);
    }

    mutable std::mutex mutex_;
    std::map<int, AudioTrack> tracks_;
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
