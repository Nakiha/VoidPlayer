#include "video_renderer/decode/demux_thread.h"
#include <spdlog/spdlog.h>

namespace vr {

DemuxThread::DemuxThread(const std::string& file_path, PacketQueue& output_queue)
    : file_path_(file_path)
    , output_queue_(output_queue)
{}

DemuxThread::~DemuxThread() {
    stop();
}

bool DemuxThread::start() {
    if (running_.load()) return false;

    // Open format context on the calling thread so stats are available immediately
    int ret = avformat_open_input(&fmt_ctx_, file_path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        spdlog::error("[DemuxThread] Failed to open input: {}", file_path_);
        return false;
    }

    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        spdlog::error("[DemuxThread] Failed to find stream info");
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    // Locate the first video stream
    stats_.video_stream_index = -1;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stats_.video_stream_index = static_cast<int>(i);
            break;
        }
    }

    if (stats_.video_stream_index < 0) {
        spdlog::error("[DemuxThread] No video stream found in {}", file_path_);
        avformat_close_input(&fmt_ctx_);
        return false;
    }

    AVStream* stream = fmt_ctx_->streams[stats_.video_stream_index];
    stats_.codec_params = stream->codecpar;
    stats_.time_base = stream->time_base;
    stats_.width = stream->codecpar->width;
    stats_.height = stream->codecpar->height;

    if (fmt_ctx_->duration != AV_NOPTS_VALUE) {
        stats_.duration_us = av_rescale_q(fmt_ctx_->duration, {1, AV_TIME_BASE}, {1, 1000000});
    }

    spdlog::info("[DemuxThread] Opened {} ({}x{}, stream={}, tb={}/{})",
                 file_path_, stats_.width, stats_.height,
                 stats_.video_stream_index,
                 stats_.time_base.num, stats_.time_base.den);

    running_.store(true);
    thread_ = std::thread(&DemuxThread::run, this);
    return true;
}

void DemuxThread::stop() {
    running_.store(false);
    output_queue_.abort();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
    }
}

void DemuxThread::seek(int64_t target_pts_us) {
    seek_target_us_.store(target_pts_us, std::memory_order_release);
    seeking_.store(true, std::memory_order_release);
}

void DemuxThread::run() {
    spdlog::info("[DemuxThread] Demux loop started");

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        spdlog::error("[DemuxThread] Failed to allocate packet");
        running_.store(false);
        return;
    }

    int stream_idx = stats_.video_stream_index;

    while (running_.load()) {
        // Handle seek request
        if (seeking_.load(std::memory_order_acquire)) {
            int64_t target_us = seek_target_us_.load(std::memory_order_acquire);
            seeking_.store(false, std::memory_order_release);

            int64_t target_tb = av_rescale_q(target_us, {1, 1000000}, stats_.time_base);
            output_queue_.flush();
            int seek_ret = av_seek_frame(fmt_ctx_, stream_idx, target_tb, AVSEEK_FLAG_BACKWARD);
            if (seek_ret < 0) {
                spdlog::error("[DemuxThread] Seek to {} us failed (ret={})", target_us, seek_ret);
            } else {
                spdlog::debug("[DemuxThread] Seeked to {} us (tb={})", target_us, target_tb);
            }
        }

        int ret = av_read_frame(fmt_ctx_, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                spdlog::info("[DemuxThread] End of file reached");
            } else {
                spdlog::error("[DemuxThread] Read error: {}", ret);
            }
            break;
        }

        // Discard non-video packets
        if (pkt->stream_index != stream_idx) {
            av_packet_unref(pkt);
            continue;
        }

        // NOTE: Do NOT convert PTS here — keep packets in stream time_base.
        // The decode thread will convert frame PTS to microseconds after decoding.
        // Double-conversion would produce wildly incorrect timestamps.

        // Push takes ownership
        AVPacket* out = av_packet_alloc();
        av_packet_ref(out, pkt);
        av_packet_unref(pkt);

        if (!output_queue_.push(out)) {
            av_packet_free(&out);
            // Queue aborted or full — don't permanently exit, just drop this packet
            continue;
        }
    }

    av_packet_free(&pkt);
    // Signal decode thread that no more packets will come
    output_queue_.abort();
    spdlog::info("[DemuxThread] Demux loop ended");
}

int64_t DemuxThread::pts_to_us(int64_t pts, AVRational tb) const {
    if (pts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
    return av_rescale_q(pts, tb, {1, 1000000});
}

} // namespace vr
