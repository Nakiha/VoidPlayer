#include "video_renderer/decode/frame_timestamp_rescaler.h"
#include "video_renderer/decode/software_frame_publisher.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

#ifndef VIDEO_TEST_DIR
#define VIDEO_TEST_DIR ""
#endif

namespace {

std::string default_media_path() {
    const std::string root = VIDEO_TEST_DIR;
    return root.empty() ? std::string{} : root + "/h264_9s_1920x1080.mp4";
}

std::string ffmpeg_error(int err) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buffer, sizeof(buffer));
    return buffer;
}

struct FormatContextDeleter {
    void operator()(AVFormatContext* ctx) const {
        if (ctx) {
            avformat_close_input(&ctx);
        }
    }
};

struct CodecContextDeleter {
    void operator()(AVCodecContext* ctx) const { avcodec_free_context(&ctx); }
};

struct PacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};

struct FrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};

bool receive_and_publish(AVCodecContext* codec_ctx,
                         AVStream* stream,
                         vr::SoftwareFrameQueuePublisher& publisher) {
    std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
    if (!frame) {
        std::cerr << "failed to allocate frame\n";
        return false;
    }

    while (true) {
        const int ret = avcodec_receive_frame(codec_ctx, frame.get());
        if (ret == 0) {
            vr::rescale_frame_timestamps_to_us(frame.get(), stream->time_base);
            return publisher.publish_bgra_frame(frame.get());
        }
        if (ret == AVERROR(EAGAIN)) {
            return false;
        }
        if (ret == AVERROR_EOF) {
            return false;
        }
        std::cerr << "avcodec_receive_frame failed: " << ffmpeg_error(ret) << "\n";
        return false;
    }
}

bool decode_first_frame_to_queue(const std::string& path, vr::TrackBuffer& buffer) {
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
        std::cerr << "failed to find video decoder\n";
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
    if (!packet) {
        std::cerr << "failed to allocate packet\n";
        return false;
    }

    vr::SoftwareFrameQueuePublisher publisher(buffer);
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
        if (receive_and_publish(codec_ctx.get(), stream, publisher)) {
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
    return receive_and_publish(codec_ctx.get(), stream, publisher);
}

double non_black_ratio(const vr::TextureFrame& frame) {
    const auto* rgba = frame.cpu_rgba_storage();
    if (!rgba || !rgba->data || rgba->data->empty()) {
        return 0.0;
    }
    int non_black = 0;
    const size_t pixels = rgba->data->size() / 4;
    for (size_t offset = 0; offset + 3 < rgba->data->size(); offset += 4) {
        const uint8_t b = (*rgba->data)[offset + 0];
        const uint8_t g = (*rgba->data)[offset + 1];
        const uint8_t r = (*rgba->data)[offset + 2];
        if (r > 4 || g > 4 || b > 4) {
            ++non_black;
        }
    }
    return pixels == 0 ? 0.0 : static_cast<double>(non_black) / static_cast<double>(pixels);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc >= 2 ? argv[1] : default_media_path();
    if (path.empty()) {
        std::cerr << "usage: software_decode_frame_queue_smoke <media-path>\n";
        return 2;
    }

    vr::TrackBuffer buffer(2, 1);
    if (!decode_first_frame_to_queue(path, buffer)) {
        std::cerr << "failed to decode and queue first frame: " << path << "\n";
        return 1;
    }

    auto frame = buffer.peek();
    if (!frame) {
        std::cerr << "queued frame missing\n";
        return 1;
    }
    if (frame->storage_kind() != vr::FrameStorageKind::CpuRgba ||
        frame->width <= 0 || frame->height <= 0) {
        std::cerr << "queued frame has invalid storage or dimensions\n";
        return 1;
    }
    if (non_black_ratio(*frame) <= 0.5) {
        std::cerr << "queued frame is unexpectedly black\n";
        return 1;
    }
    if (!buffer.advance() || buffer.last_presented_pts_us() < 0) {
        std::cerr << "queued frame did not preserve TrackBuffer presentation semantics\n";
        return 1;
    }

    std::cout << "queued decoded frame " << frame->width << "x" << frame->height
              << " pts_us=" << frame->pts_us
              << " non_black=" << non_black_ratio(*frame) << "\n";
    return 0;
}
