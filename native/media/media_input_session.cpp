#include "media/media_input_session.h"

#include "media/private_cdn_flv_demuxer.h"
#include "renderer/decode/frame_color_metadata.h"

#include <algorithm>
#include <cerrno>
#include <utility>

extern "C" {
#include <libavutil/error.h>
}

namespace vr {
namespace {

int64_t steady_clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string ffmpeg_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

void fill_codec_names(MediaInputStats& stats, AVCodecID codec_id) {
    stats.codec_name = avcodec_get_name(codec_id);
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codec_id);
    if (descriptor && descriptor->long_name) {
        stats.codec_long_name = descriptor->long_name;
    }
}

} // namespace

MediaInputSession::MediaInputSession() = default;

MediaInputSession::~MediaInputSession() {
    close();
}

bool MediaInputSession::open(
    const std::string& path,
    const MediaInputOpenOptions& options,
    std::string& error) {
    close();
    error.clear();
    path_ = path;
    interrupt_requested_ = options.interrupt_requested;

    const auto timeout = std::max(
        options.open_timeout,
        std::chrono::milliseconds::zero());
    open_deadline_ns_.store(
        timeout.count() > 0
            ? steady_clock_ns() +
                  std::chrono::duration_cast<std::chrono::nanoseconds>(
                      timeout).count()
            : 0,
        std::memory_order_release);

    auto fail = [this, &error](std::string message) {
        error = std::move(message);
        close();
        return false;
    };

    if (PrivateCdnFlvDemuxer::probe(path_)) {
        auto demuxer = std::make_unique<PrivateCdnFlvDemuxer>();
        if (!demuxer->open(path_)) {
            return fail("private CDN FLV input open failed");
        }
        private_demuxer_ = std::move(demuxer);
        stats_ = private_demuxer_->stats();
        open_deadline_ns_.store(0, std::memory_order_release);
        return true;
    }

    format_context_ = AvFormatContextOwner::allocate();
    if (!format_context_) {
        return fail("avformat_alloc_context failed");
    }
    format_context_->interrupt_callback.callback =
        &MediaInputSession::interrupt_callback;
    format_context_->interrupt_callback.opaque = this;

    int result = avformat_open_input(
        format_context_.mutable_address(),
        path_.c_str(),
        nullptr,
        nullptr);
    if (result < 0) {
        return fail(
            "avformat_open_input failed: " + ffmpeg_error(result));
    }
    result = avformat_find_stream_info(format_context_.get(), nullptr);
    if (result < 0) {
        return fail(
            "avformat_find_stream_info failed: " +
            ffmpeg_error(result));
    }
    open_deadline_ns_.store(0, std::memory_order_release);
    if (should_interrupt()) {
        return fail("input open cancelled");
    }

    for (unsigned int i = 0;
         i < format_context_->nb_streams;
         ++i) {
        AVCodecParameters* codec_params =
            format_context_->streams[i]->codecpar;
        if (stats_.video_stream_index < 0 &&
            codec_params->codec_type == AVMEDIA_TYPE_VIDEO) {
            stats_.video_stream_index = static_cast<int>(i);
        } else if (stats_.audio_stream_index < 0 &&
                   codec_params->codec_type == AVMEDIA_TYPE_AUDIO) {
            stats_.audio_stream_index = static_cast<int>(i);
        }
    }

    if (stats_.video_stream_index >= 0) {
        AVStream* stream =
            format_context_->streams[stats_.video_stream_index];
        if (!stats_.set_video_codec_params(stream->codecpar)) {
            return fail("failed to copy video codec parameters");
        }
        stats_.time_base = stream->time_base;
        stats_.width = stream->codecpar->width;
        stats_.height = stream->codecpar->height;
        stats_.color =
            color_info_from_av_codec_parameters(stream->codecpar);
        fill_codec_names(stats_, stream->codecpar->codec_id);
        if (stream->start_time != AV_NOPTS_VALUE) {
            stats_.start_time_us = av_rescale_q(
                stream->start_time,
                stream->time_base,
                AVRational{1, 1000000});
        } else if (format_context_->start_time != AV_NOPTS_VALUE) {
            stats_.start_time_us = av_rescale_q(
                format_context_->start_time,
                AVRational{1, AV_TIME_BASE},
                AVRational{1, 1000000});
        }
        stats_.bit_rate = stream->codecpar->bit_rate > 0
            ? stream->codecpar->bit_rate
            : format_context_->bit_rate;
        if (stream->codecpar->sample_aspect_ratio.num > 0 &&
            stream->codecpar->sample_aspect_ratio.den > 0) {
            stats_.sar_num =
                stream->codecpar->sample_aspect_ratio.num;
            stats_.sar_den =
                stream->codecpar->sample_aspect_ratio.den;
        }
    }

    if (format_context_->duration != AV_NOPTS_VALUE) {
        stats_.duration_us = av_rescale_q(
            format_context_->duration,
            AVRational{1, AV_TIME_BASE},
            AVRational{1, 1000000});
    }
    if (format_context_->iformat) {
        if (format_context_->iformat->long_name) {
            stats_.format_name =
                format_context_->iformat->long_name;
        } else if (format_context_->iformat->name) {
            stats_.format_name =
                format_context_->iformat->name;
        }
    }

    if (stats_.audio_stream_index >= 0) {
        AVStream* stream =
            format_context_->streams[stats_.audio_stream_index];
        if (stats_.set_audio_codec_params(stream->codecpar)) {
            stats_.audio_time_base = stream->time_base;
            stats_.sample_rate = stream->codecpar->sample_rate;
            stats_.channels =
                stream->codecpar->ch_layout.nb_channels;
        } else {
            stats_.audio_stream_index = -1;
        }
    }
    return true;
}

void MediaInputSession::close() {
    open_deadline_ns_.store(0, std::memory_order_release);
    private_demuxer_.reset();
    format_context_.reset();
    stats_ = MediaInputStats{};
    interrupt_requested_ = {};
    path_.clear();
}

bool MediaInputSession::is_open() const noexcept {
    return format_context_ || private_demuxer_ != nullptr;
}

bool MediaInputSession::uses_private_demuxer() const noexcept {
    return private_demuxer_ != nullptr;
}

const char* MediaInputSession::backend_name() const noexcept {
    return private_demuxer_ ? "private-cdn-flv" : "libavformat";
}

int MediaInputSession::read_packet(AVPacket* packet) {
    if (!packet) {
        return AVERROR(EINVAL);
    }
    if (private_demuxer_) {
        return private_demuxer_->read_packet(packet);
    }
    if (format_context_) {
        return av_read_frame(format_context_.get(), packet);
    }
    return AVERROR(EINVAL);
}

int MediaInputSession::seek(
    int stream_index,
    int64_t timestamp,
    int flags) {
    if (private_demuxer_) {
        return private_demuxer_->seek(stream_index, timestamp);
    }
    if (format_context_) {
        return av_seek_frame(
            format_context_.get(),
            stream_index,
            timestamp,
            flags);
    }
    return AVERROR(EINVAL);
}

void MediaInputSession::flush() {
    if (private_demuxer_) {
        private_demuxer_->flush();
    } else if (format_context_) {
        avformat_flush(format_context_.get());
    }
}

int MediaInputSession::best_stream_index(
    AVMediaType media_type) const {
    if (private_demuxer_) {
        if (media_type == AVMEDIA_TYPE_VIDEO) {
            return stats_.video_stream_index;
        }
        if (media_type == AVMEDIA_TYPE_AUDIO) {
            return stats_.audio_stream_index;
        }
        return AVERROR_STREAM_NOT_FOUND;
    }
    if (!format_context_) {
        return AVERROR_STREAM_NOT_FOUND;
    }
    return av_find_best_stream(
        format_context_.get(),
        media_type,
        -1,
        -1,
        nullptr,
        0);
}

AVRational MediaInputSession::time_base_for_stream(
    int stream_index) const {
    if (private_demuxer_) {
        return private_demuxer_->time_base_for_stream(
            stream_index);
    }
    if (!format_context_ || stream_index < 0 ||
        stream_index >=
            static_cast<int>(format_context_->nb_streams)) {
        return AVRational{0, 1};
    }
    return format_context_->streams[stream_index]->time_base;
}

AVRational MediaInputSession::frame_rate_for_stream(
    int stream_index) const {
    if (private_demuxer_ || !format_context_ ||
        stream_index < 0 ||
        stream_index >=
            static_cast<int>(format_context_->nb_streams)) {
        return AVRational{0, 1};
    }
    AVStream* stream =
        format_context_->streams[stream_index];
    return av_guess_frame_rate(
        format_context_.get(), stream, nullptr);
}

AVCodecParameters* MediaInputSession::codec_parameters(
    int stream_index) const {
    if (private_demuxer_) {
        if (stream_index == stats_.video_stream_index) {
            return stats_.codec_params;
        }
        if (stream_index == stats_.audio_stream_index) {
            return stats_.audio_codec_params;
        }
        return nullptr;
    }
    if (!format_context_ || stream_index < 0 ||
        stream_index >=
            static_cast<int>(format_context_->nb_streams)) {
        return nullptr;
    }
    return format_context_->streams[stream_index]->codecpar;
}

int MediaInputSession::interrupt_callback(void* opaque) {
    const auto* session =
        static_cast<const MediaInputSession*>(opaque);
    return session && session->should_interrupt() ? 1 : 0;
}

bool MediaInputSession::should_interrupt() const {
    if (interrupt_requested_ && interrupt_requested_()) {
        return true;
    }
    const int64_t deadline =
        open_deadline_ns_.load(std::memory_order_acquire);
    return deadline > 0 && steady_clock_ns() > deadline;
}

} // namespace vr
