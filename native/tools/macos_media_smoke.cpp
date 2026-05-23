#include "media/demux_thread.h"
#include "media/packet_queue.h"
#include "media/seek_controller.h"
#include "tools/test_video_assets.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#ifndef VIDEO_TEST_DIR
#define VIDEO_TEST_DIR ""
#endif

namespace {

std::string default_media_path() {
    return vp_tools::h264_smoke_video_path(VIDEO_TEST_DIR);
}

std::string ffmpeg_error(int err) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buffer, sizeof(buffer));
    return buffer;
}

struct DecodedFrameInfo {
    int width = 0;
    int height = 0;
    AVPixelFormat format = AV_PIX_FMT_NONE;
    int64_t pts = AV_NOPTS_VALUE;
};

struct FormatContextDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) {
            avformat_close_input(&ctx);
        }
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* ctx) const {
        avcodec_free_context(&ctx);
    }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const {
        av_packet_free(&packet);
    }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const {
        av_frame_free(&frame);
    }
};

bool receive_first_frame(AVCodecContext* codec_ctx,
                         AVFrame* frame,
                         DecodedFrameInfo& out) {
    while (true) {
        const int ret = avcodec_receive_frame(codec_ctx, frame);
        if (ret == 0) {
            out.width = frame->width;
            out.height = frame->height;
            out.format = static_cast<AVPixelFormat>(frame->format);
            out.pts = frame->pts;
            return out.width > 0 && out.height > 0 && out.format != AV_PIX_FMT_NONE;
        }
        if (ret == AVERROR(EAGAIN)) {
            return false;
        }
        if (ret == AVERROR_EOF) {
            return false;
        }

        std::cerr << "failed to receive decoded frame: " << ffmpeg_error(ret) << "\n";
        return false;
    }
}

bool decode_one_video_frame(const std::string& path, DecodedFrameInfo& out) {
    AVFormatContext* raw_format_ctx = nullptr;
    int ret = avformat_open_input(&raw_format_ctx, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::cerr << "avformat_open_input failed: " << ffmpeg_error(ret) << "\n";
        return false;
    }
    std::unique_ptr<AVFormatContext, FormatContextDeleter> format_ctx(raw_format_ctx);

    ret = avformat_find_stream_info(format_ctx.get(), nullptr);
    if (ret < 0) {
        std::cerr << "avformat_find_stream_info failed: " << ffmpeg_error(ret) << "\n";
        return false;
    }

    const int stream_index =
        av_find_best_stream(format_ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index < 0) {
        std::cerr << "failed to find video stream: " << ffmpeg_error(stream_index) << "\n";
        return false;
    }

    AVStream* stream = format_ctx->streams[stream_index];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        std::cerr << "failed to find decoder for codec id "
                  << static_cast<int>(stream->codecpar->codec_id) << "\n";
        return false;
    }

    std::unique_ptr<AVCodecContext, CodecContextDeleter> codec_ctx(
        avcodec_alloc_context3(codec));
    if (!codec_ctx) {
        std::cerr << "failed to allocate codec context\n";
        return false;
    }

    ret = avcodec_parameters_to_context(codec_ctx.get(), stream->codecpar);
    if (ret < 0) {
        std::cerr << "avcodec_parameters_to_context failed: " << ffmpeg_error(ret) << "\n";
        return false;
    }

    ret = avcodec_open2(codec_ctx.get(), codec, nullptr);
    if (ret < 0) {
        std::cerr << "avcodec_open2 failed: " << ffmpeg_error(ret) << "\n";
        return false;
    }

    std::unique_ptr<AVPacket, PacketDeleter> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (!packet || !frame) {
        std::cerr << "failed to allocate packet/frame\n";
        return false;
    }

    while ((ret = av_read_frame(format_ctx.get(), packet.get())) >= 0) {
        if (packet->stream_index != stream_index) {
            av_packet_unref(packet.get());
            continue;
        }

        ret = avcodec_send_packet(codec_ctx.get(), packet.get());
        av_packet_unref(packet.get());
        if (ret < 0) {
            std::cerr << "avcodec_send_packet failed: " << ffmpeg_error(ret) << "\n";
            return false;
        }
        if (receive_first_frame(codec_ctx.get(), frame.get(), out)) {
            return true;
        }
    }

    if (ret != AVERROR_EOF) {
        std::cerr << "av_read_frame failed: " << ffmpeg_error(ret) << "\n";
        return false;
    }

    ret = avcodec_send_packet(codec_ctx.get(), nullptr);
    if (ret < 0 && ret != AVERROR_EOF) {
        std::cerr << "decoder flush failed: " << ffmpeg_error(ret) << "\n";
        return false;
    }
    return receive_first_frame(codec_ctx.get(), frame.get(), out);
}

} // namespace

int main(int argc, char** argv) {
    const std::string path = argc >= 2 ? argv[1] : default_media_path();
    if (path.empty()) {
        std::cerr << "usage: macos_media_smoke <media-path>\n";
        return 2;
    }

    vr::SeekController seek_controller;
    vr::PacketQueue video_queue;
    vr::DemuxThread demux(path, video_queue, seek_controller);
    if (!demux.open()) {
        std::cerr << "failed to open media: " << path << "\n";
        return 1;
    }

    const auto& stats = demux.stats();
    if (stats.video_stream_index < 0 || stats.width <= 0 || stats.height <= 0) {
        std::cerr << "missing video metadata for: " << path << "\n";
        return 1;
    }
    if (stats.duration_us <= 0) {
        std::cerr << "missing duration metadata for: " << path << "\n";
        return 1;
    }

    DecodedFrameInfo frame;
    if (!decode_one_video_frame(path, frame)) {
        std::cerr << "failed to decode a software video frame: " << path << "\n";
        return 1;
    }
    if (frame.width != stats.width || frame.height != stats.height) {
        std::cerr << "decoded frame size mismatch: metadata=" << stats.width << "x"
                  << stats.height << " frame=" << frame.width << "x" << frame.height << "\n";
        return 1;
    }

    const char* pix_fmt_name = av_get_pix_fmt_name(frame.format);
    std::cout << "opened " << path << "\n"
              << "codec=" << stats.codec_name << "\n"
              << "size=" << stats.width << "x" << stats.height << "\n"
              << "duration_us=" << stats.duration_us << "\n"
              << "first_frame_format=" << (pix_fmt_name ? pix_fmt_name : "unknown") << "\n"
              << "first_frame_pts=" << frame.pts << "\n";
    return 0;
}
