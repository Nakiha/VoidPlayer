#include "bench_common.h"
#include <chrono>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

BenchResult bench_demux_decode(const std::string& path) {
    BenchResult result;
    result.name = "Stage 2: Demux + Decode";

    AVFormatContext* fmt_ctx = nullptr;
    avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr);
    avformat_find_stream_info(fmt_ctx, nullptr);

    int video_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = static_cast<int>(i);
            break;
        }
    }

    AVStream* stream = fmt_ctx->streams[video_idx];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, stream->codecpar);
    avcodec_open2(codec_ctx, codec, nullptr);

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();

    auto t0 = std::chrono::high_resolution_clock::now();

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != video_idx) {
            av_packet_unref(pkt);
            continue;
        }
        result.total_packets++;

        int ret = avcodec_send_packet(codec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
            result.total_frames++;
            result.bytes_processed += frame->width * frame->height * 1.5;
            av_frame_unref(frame);
        }
    }

    avcodec_send_packet(codec_ctx, nullptr);
    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
        result.total_frames++;
        result.bytes_processed += frame->width * frame->height * 1.5;
        av_frame_unref(frame);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.fps = result.total_frames / (result.elapsed_ms / 1000.0);
    result.packets_per_sec = result.total_packets / (result.elapsed_ms / 1000.0);
    result.bytes_processed /= (1024.0 * 1024.0);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return result;
}
