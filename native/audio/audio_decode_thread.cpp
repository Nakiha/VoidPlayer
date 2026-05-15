#include "audio/audio_decode_thread.h"

#include "audio/audio_constants.h"
#include "audio/pcm_buffer.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace vr {

AudioDecodeThread::AudioDecodeThread(PacketQueue& input_queue,
                                     PcmBuffer& output_buffer,
                                     const AVCodecParameters* codec_params,
                                     AVRational time_base)
    : input_queue_(input_queue)
    , output_buffer_(output_buffer)
    , codec_params_(codec_params)
    , time_base_(time_base) {}

AudioDecodeThread::~AudioDecodeThread() {
    stop();
}

bool AudioDecodeThread::start() {
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

void AudioDecodeThread::stop() {
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

void AudioDecodeThread::set_paused(bool paused) {
    decode_paused_.store(paused);
}

void AudioDecodeThread::notify_seek(int64_t target_pts_us, SeekType type) {
    seek_pending_.store(true);
    output_buffer_.begin_seek(target_pts_us, type);
}

bool AudioDecodeThread::init_resampler() {
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, kAudioOutputChannels);
    int ret = swr_alloc_set_opts2(
        &swr_,
        &out_layout,
        AV_SAMPLE_FMT_S16,
        kAudioOutputSampleRate,
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

void AudioDecodeThread::flush_after_seek_if_needed() {
    if (!seek_pending_.exchange(false)) return;
    avcodec_flush_buffers(codec_ctx_);
    if (swr_) swr_close(swr_);
    if (swr_) swr_init(swr_);
    output_buffer_.flush();
}

void AudioDecodeThread::receive_frames(AVFrame* frame) {
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
                kAudioOutputSampleRate,
                codec_ctx_->sample_rate,
                AV_ROUND_UP));
        std::vector<int16_t> pcm(static_cast<size_t>(out_capacity) * kAudioOutputChannels);
        uint8_t* out_data[] = {reinterpret_cast<uint8_t*>(pcm.data())};
        int out_samples = swr_convert(
            swr_,
            out_data,
            out_capacity,
            const_cast<const uint8_t**>(frame->extended_data),
            frame->nb_samples);
        if (out_samples > 0) {
            const size_t out_frames = static_cast<size_t>(out_samples);
            output_buffer_.push(
                pcm.data(),
                out_frames,
                frame_pts_us(frame),
                frames_to_duration_us(out_frames),
                output_buffer_.current_serial());
        }
        av_frame_unref(frame);
    }
}

void AudioDecodeThread::run() {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return;
    while (running_.load()) {
        if (decode_paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        flush_after_seek_if_needed();
        PacketPopResult packet_result = input_queue_.pop();
        AVPacket* pkt = packet_result.packet;
        if (packet_result.status != PacketPopStatus::Packet || !pkt) {
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

int64_t AudioDecodeThread::frame_pts_us(const AVFrame* frame) const {
    if (!frame) return kAudioNoPts;
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
        pts = frame->pts;
    }
    if (pts == AV_NOPTS_VALUE) {
        return kAudioNoPts;
    }
    return av_rescale_q(pts, time_base_, AVRational{1, AV_TIME_BASE});
}

int64_t AudioDecodeThread::frames_to_duration_us(size_t frames) {
    return static_cast<int64_t>(
        (static_cast<int64_t>(frames) * 1000000LL) /
        static_cast<int64_t>(kAudioOutputSampleRate));
}

} // namespace vr
