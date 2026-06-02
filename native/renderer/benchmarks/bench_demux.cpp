#include "bench_common.h"
#include <chrono>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

BenchResult bench_demux_only(const std::string& path) {
    BenchResult result;
    result.name = "Stage 1: Demux Only";

    AVFormatContext* fmt_ctx = nullptr;
    int ret = avformat_open_input(&fmt_ctx, path.c_str(), nullptr, nullptr);
    if (ret < 0) { std::cerr << "Failed to open: " << path << "\n"; return result; }
    avformat_find_stream_info(fmt_ctx, nullptr);

    int video_idx = -1;
    for (unsigned i = 0; i < fmt_ctx->nb_streams; ++i) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_idx = static_cast<int>(i);
            break;
        }
    }

    AVPacket* pkt = av_packet_alloc();
    auto t0 = std::chrono::high_resolution_clock::now();

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_idx) {
            result.total_packets++;
            result.bytes_processed += pkt->size;
        }
        av_packet_unref(pkt);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    result.packets_per_sec = result.total_packets / (result.elapsed_ms / 1000.0);
    result.bytes_processed /= (1024.0 * 1024.0);

    av_packet_free(&pkt);
    avformat_close_input(&fmt_ctx);
    return result;
}
