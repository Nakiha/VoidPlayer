#include "video_renderer/decode/demux_thread.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace vr {

DemuxThread::DemuxThread(const std::string& file_path, PacketQueue& output_queue,
                         SeekController& seek_controller)
    : file_path_(file_path)
    , output_queue_(output_queue)
    , seek_controller_(seek_controller)
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
    // Extract Sample Aspect Ratio for correct display aspect ratio
    if (stream->codecpar->sample_aspect_ratio.num > 0 &&
        stream->codecpar->sample_aspect_ratio.den > 0) {
        stats_.sar_num = stream->codecpar->sample_aspect_ratio.num;
        stats_.sar_den = stream->codecpar->sample_aspect_ratio.den;
    }

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
    spdlog::info("[DemuxThread] stop() begin for {}", file_path_);
    running_.store(false);
    output_queue_.abort();
    if (thread_.joinable()) {
        spdlog::info("[DemuxThread] stop() waiting for join: {}", file_path_);
        thread_.join();
        spdlog::info("[DemuxThread] stop() joined: {}", file_path_);
    }
    if (fmt_ctx_) {
        spdlog::info("[DemuxThread] stop() closing input: {}", file_path_);
        avformat_close_input(&fmt_ctx_);
    }
    spdlog::info("[DemuxThread] stop() end for {}", file_path_);
}

void DemuxThread::set_seek_callback(SeekCallback cb) {
    seek_callback_ = std::move(cb);
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
    bool eof_reached = false;
    int packets_pushed = 0;  // Count packets pushed after each seek

    while (running_.load()) {
        // Handle seek request from SeekController (atomically take + clear)
        auto req_opt = seek_controller_.take_pending();
        if (req_opt.has_value()) {
            auto req = req_opt.value();

            spdlog::info("[DemuxThread] Executing seek: target={:.3f}s, type={}, pq_size={}",
                         req.target_pts_us / 1e6,
                         req.type == SeekType::Exact ? "Exact" : "Keyframe",
                         output_queue_.size());

            output_queue_.flush();
            output_queue_.clear_eof();

            int64_t target_tb = av_rescale_q(req.target_pts_us, {1, 1000000}, stats_.time_base);
            int seek_ret = av_seek_frame(fmt_ctx_, stream_idx, target_tb, AVSEEK_FLAG_BACKWARD);
            if (seek_ret < 0) {
                spdlog::error("[DemuxThread] av_seek_frame FAILED: target={:.3f}s, ret={:#x}",
                             req.target_pts_us / 1e6, static_cast<unsigned>(seek_ret));
            } else {
                // Clear demuxer EOF/read-ahead state so av_read_frame() starts
                // producing packets again after seeks from end-of-file.
                avformat_flush(fmt_ctx_);
                spdlog::info("[DemuxThread] av_seek_frame OK: target={:.3f}s", req.target_pts_us / 1e6);
            }

            if (seek_callback_) {
                spdlog::info("[DemuxThread] Invoking seek callback -> DecodeThread");
                seek_callback_(req.target_pts_us, req.type);
            }

            eof_reached = false;
            packets_pushed = 0;
            spdlog::info("[DemuxThread] Seek complete, resuming packet reads");
            continue;
        }

        if (eof_reached) {
            // Wait for seek or shutdown
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        int ret = av_read_frame(fmt_ctx_, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                spdlog::info("[DemuxThread] EOF reached after {} packets, waiting for seek",
                             packets_pushed);
                output_queue_.signal_eof();
                eof_reached = true;
                continue;
            }
            spdlog::error("[DemuxThread] Read error: {:#x}", static_cast<unsigned>(ret));
            break;
        }

        if (pkt->stream_index != stream_idx) {
            av_packet_unref(pkt);
            continue;
        }

        // NOTE: Do NOT convert PTS here — keep packets in stream time_base.
        // The decode thread will convert frame PTS to microseconds after decoding.
        // Double-conversion would produce wildly incorrect timestamps.

        // Push takes ownership
        AVPacket* out = av_packet_clone(pkt);
        av_packet_unref(pkt);

        if (!output_queue_.push(out)) {
            av_packet_free(&out);
            // Queue aborted or full — don't permanently exit, just drop this packet
            continue;
        }
        ++packets_pushed;
    }

    av_packet_free(&pkt);
    // Signal decode thread that no more packets will come
    output_queue_.abort();
    spdlog::info("[DemuxThread] Demux loop ended");
}

} // namespace vr
