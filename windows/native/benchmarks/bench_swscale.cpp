#include "bench_common.h"
#include <chrono>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

BenchResult bench_demux_decode_sws(const std::string& path) {
    BenchResult result;
    result.name = "Stage 3: Demux + Decode + sws_scale (YUV->RGBA)";

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

    int w = codec_ctx->width;
    int h = codec_ctx->height;

    SwsContext* sws = sws_getContext(w, h, codec_ctx->pix_fmt,
                                      w, h, AV_PIX_FMT_RGBA,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);

    size_t stride = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> rgba_buf(stride * h);

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
            uint8_t* dst_slices[1] = { rgba_buf.data() };
            int dst_stride[1] = { static_cast<int>(stride) };

            sws_scale(sws,
                frame->data, frame->linesize,
                0, h, dst_slices, dst_stride);

            result.total_frames++;
            result.bytes_processed += w * h * 4;
            av_frame_unref(frame);
        }
    }

    // Flush
    avcodec_send_packet(codec_ctx, nullptr);
    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
        uint8_t* dst_slices[1] = { rgba_buf.data() };
        int dst_stride[1] = { static_cast<int>(stride) };
        sws_scale(sws, frame->data, frame->linesize, 0, h, dst_slices, dst_stride);
        result.total_frames++;
        result.bytes_processed += w * h * 4;
        av_frame_unref(frame);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.fps = result.total_frames / (result.elapsed_ms / 1000.0);
    result.packets_per_sec = result.total_packets / (result.elapsed_ms / 1000.0);
    result.bytes_processed /= (1024.0 * 1024.0);

    sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    return result;
}
