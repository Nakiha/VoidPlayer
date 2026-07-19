#pragma once

#include "media/ffmpeg_lifetime.h"
#include "renderer/frame/frame_storage.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace vr {

class PrivateCdnFlvDemuxer;

namespace detail {
inline std::shared_ptr<AVCodecParameters> clone_codec_params(
    const AVCodecParameters* source) {
    if (!source) {
        return {};
    }

    AVCodecParameters* copy = avcodec_parameters_alloc();
    if (!copy) {
        return {};
    }
    if (avcodec_parameters_copy(copy, source) < 0) {
        avcodec_parameters_free(&copy);
        return {};
    }

    return std::shared_ptr<AVCodecParameters>(
        copy,
        [](AVCodecParameters* params) {
            avcodec_parameters_free(&params);
        });
}
} // namespace detail

struct MediaInputStats {
    int video_stream_index = -1;
    int audio_stream_index = -1;
    int64_t duration_us = 0;
    int64_t start_time_us = 0;
    int64_t bit_rate = 0;
    int width = 0;
    int height = 0;
    std::string format_name;
    std::string codec_name;
    std::string codec_long_name;
    VideoColorInfo color;
    AVRational time_base = {0, 1};
    AVRational audio_time_base = {0, 1};
    int sar_num = 1;
    int sar_den = 1;
    int sample_rate = 0;
    int channels = 0;
    /// Owned snapshots of the stream codec parameters. The raw pointers below
    /// are convenience aliases and remain valid while this stats copy lives.
    std::shared_ptr<AVCodecParameters> codec_params_owner;
    std::shared_ptr<AVCodecParameters> audio_codec_params_owner;
    AVCodecParameters* codec_params = nullptr;
    AVCodecParameters* audio_codec_params = nullptr;

    bool set_video_codec_params(const AVCodecParameters* source) {
        codec_params_owner = detail::clone_codec_params(source);
        codec_params = codec_params_owner.get();
        return codec_params != nullptr;
    }

    bool set_audio_codec_params(const AVCodecParameters* source) {
        audio_codec_params_owner = detail::clone_codec_params(source);
        audio_codec_params = audio_codec_params_owner.get();
        return audio_codec_params != nullptr;
    }
};

// Preserve the public playback name while the storage moves to the shared
// media-input layer.
using DemuxStats = MediaInputStats;

struct MediaInputOpenOptions {
    std::chrono::milliseconds open_timeout{15000};
    std::function<bool()> interrupt_requested;
};

/// Synchronous, playback-agnostic FFmpeg input session.
///
/// This class owns input probing, stream metadata, packet reads and seeking.
/// It deliberately has no worker thread, packet queue, loop or playback clock;
/// GUI playback and offline analysis provide those policies themselves.
class MediaInputSession {
public:
    MediaInputSession();
    ~MediaInputSession();

    MediaInputSession(const MediaInputSession&) = delete;
    MediaInputSession& operator=(const MediaInputSession&) = delete;

    bool open(const std::string& path,
              const MediaInputOpenOptions& options,
              std::string& error);
    void close();

    bool is_open() const noexcept;
    bool uses_private_demuxer() const noexcept;
    const char* backend_name() const noexcept;

    int read_packet(AVPacket* packet);
    int seek(int stream_index,
             int64_t timestamp,
             int flags = AVSEEK_FLAG_BACKWARD);
    void flush();

    int best_stream_index(AVMediaType media_type) const;
    AVRational time_base_for_stream(int stream_index) const;
    AVRational frame_rate_for_stream(int stream_index) const;
    AVCodecParameters* codec_parameters(int stream_index) const;

    const MediaInputStats& stats() const noexcept { return stats_; }
    AVFormatContext* format_context() const noexcept {
        return format_context_.get();
    }

private:
    static int interrupt_callback(void* opaque);
    bool should_interrupt() const;

    std::string path_;
    AvFormatContextOwner format_context_;
    std::unique_ptr<PrivateCdnFlvDemuxer> private_demuxer_;
    MediaInputStats stats_;
    std::function<bool()> interrupt_requested_;
    std::atomic<int64_t> open_deadline_ns_{0};
};

} // namespace vr
