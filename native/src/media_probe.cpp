#include "voidview_native/media_probe.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
}

namespace voidview {

MediaInfo probe_file(const std::string& url) {
    MediaInfo info;

    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, url.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errbuf[256] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        info.error_message = "Failed to open input: " + std::string(errbuf);
        return info;
    }

    // Get stream info
    ret = avformat_find_stream_info(fmt_ctx, nullptr);
    if (ret < 0) {
        char errbuf[256] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        info.error_message = "Failed to find stream info: " + std::string(errbuf);
        avformat_close_input(&fmt_ctx);
        return info;
    }

    info.valid = true;
    info.format_name = fmt_ctx->iformat->name ? fmt_ctx->iformat->name : "";
    info.format_long_name = fmt_ctx->iformat->long_name ? fmt_ctx->iformat->long_name : "";
    info.bit_rate = fmt_ctx->bit_rate;

    // Duration
    if (fmt_ctx->duration != AV_NOPTS_VALUE) {
        info.duration_ms = fmt_ctx->duration / AV_TIME_BASE * 1000;
    }

    // Check seekable
    if (fmt_ctx->iformat->flags & AVFMT_NOBINSEARCH) {
        info.seekable = false;
    }

    // Find video stream
    info.video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (info.video_stream_index >= 0) {
        AVStream* video_stream = fmt_ctx->streams[info.video_stream_index];
        AVCodecParameters* codecpar = video_stream->codecpar;

        info.width = codecpar->width;
        info.height = codecpar->height;

        // Get codec name
        const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
        if (codec) {
            info.codec_name = codec->name ? codec->name : "";
        }

        // Calculate FPS
        if (video_stream->avg_frame_rate.den > 0 && video_stream->avg_frame_rate.num > 0) {
            info.fps = av_q2d(video_stream->avg_frame_rate);
        } else if (video_stream->r_frame_rate.den > 0 && video_stream->r_frame_rate.num > 0) {
            info.fps = av_q2d(video_stream->r_frame_rate);
        }

        // Get pixel format name
        const char* pix_fmt_name = av_get_pix_fmt_name((AVPixelFormat)codecpar->format);
        if (pix_fmt_name) {
            info.pixel_format = pix_fmt_name;
        }
    }

    // Find audio stream
    info.audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    info.has_audio = (info.audio_stream_index >= 0);

    avformat_close_input(&fmt_ctx);
    return info;
}

} // namespace voidview
