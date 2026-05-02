#include "media/demux_thread.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace vr {

namespace {
int64_t steady_clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

DemuxThread::DemuxThread(const std::string& file_path,
                         SeekController& seek_controller)
    : file_path_(file_path)
    , seek_controller_(seek_controller)
{}

DemuxThread::DemuxThread(const std::string& file_path, PacketQueue& output_queue,
                         SeekController& seek_controller)
    : DemuxThread(file_path, seek_controller)
{
    add_output(DemuxStreamKind::Video, output_queue);
}

DemuxThread::~DemuxThread() {
    stop();
}

bool DemuxThread::start() {
    if (running_.load()) return false;

    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        spdlog::error("[DemuxThread] Failed to allocate format context: {}", file_path_);
        return false;
    }
    running_.store(true, std::memory_order_release);
    open_deadline_ns_.store(
        steady_clock_ns() + std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::seconds(15)).count(),
        std::memory_order_release);
    fmt_ctx_->interrupt_callback.callback = &DemuxThread::interrupt_callback;
    fmt_ctx_->interrupt_callback.opaque = this;

    // Open format context on the calling thread so stats are available immediately.
    // Install the interrupt callback before open/find_stream_info so stop() or an
    // open timeout can break blocked probes.
    int ret = avformat_open_input(&fmt_ctx_, file_path_.c_str(), nullptr, nullptr);
    if (ret < 0) {
        spdlog::error("[DemuxThread] Failed to open input: {}", file_path_);
        running_.store(false, std::memory_order_release);
        avformat_close_input(&fmt_ctx_);
        open_deadline_ns_.store(0, std::memory_order_release);
        return false;
    }
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        spdlog::error("[DemuxThread] Failed to find stream info");
        running_.store(false, std::memory_order_release);
        avformat_close_input(&fmt_ctx_);
        open_deadline_ns_.store(0, std::memory_order_release);
        return false;
    }
    open_deadline_ns_.store(0, std::memory_order_release);

    // Locate the first video stream. Audio is discovered now too, but it is
    // only routed when an audio output queue is registered by the owner.
    stats_.video_stream_index = -1;
    stats_.audio_stream_index = -1;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVCodecParameters* codecpar = fmt_ctx_->streams[i]->codecpar;
        if (stats_.video_stream_index < 0 &&
            codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stats_.video_stream_index = static_cast<int>(i);
        } else if (stats_.audio_stream_index < 0 &&
                   codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            stats_.audio_stream_index = static_cast<int>(i);
        }
    }

    if (output_routes_.empty()) {
        spdlog::error("[DemuxThread] No output routes registered for {}", file_path_);
        running_.store(false, std::memory_order_release);
        avformat_close_input(&fmt_ctx_);
        open_deadline_ns_.store(0, std::memory_order_release);
        return false;
    }

    if (stats_.video_stream_index >= 0) {
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
    }

    if (fmt_ctx_->duration != AV_NOPTS_VALUE) {
        stats_.duration_us = av_rescale_q(fmt_ctx_->duration, {1, AV_TIME_BASE}, {1, 1000000});
    }

    if (stats_.audio_stream_index >= 0) {
        AVStream* audio_stream = fmt_ctx_->streams[stats_.audio_stream_index];
        stats_.audio_codec_params = audio_stream->codecpar;
        stats_.audio_time_base = audio_stream->time_base;
        stats_.sample_rate = audio_stream->codecpar->sample_rate;
        stats_.channels = audio_stream->codecpar->ch_layout.nb_channels;
    }

    for (auto& route : output_routes_) {
        route.stream_index = stream_index_for_kind(route.kind);
        if (route.stream_index < 0) {
            if (route.optional) {
                continue;
            }
            spdlog::error("[DemuxThread] Requested output stream is missing in {}", file_path_);
            running_.store(false, std::memory_order_release);
            avformat_close_input(&fmt_ctx_);
            open_deadline_ns_.store(0, std::memory_order_release);
            return false;
        }
    }

    spdlog::info("[DemuxThread] Opened {} ({}x{}, stream={}, tb={}/{})",
                 file_path_, stats_.width, stats_.height,
                 stats_.video_stream_index,
                 stats_.time_base.num, stats_.time_base.den);

    thread_ = std::thread(&DemuxThread::run, this);
    return true;
}

int DemuxThread::interrupt_callback(void* opaque) {
    auto* self = static_cast<DemuxThread*>(opaque);
    if (!self) return 0;
    if (!self->running_.load(std::memory_order_acquire)) {
        return 1;
    }
    const int64_t deadline = self->open_deadline_ns_.load(std::memory_order_acquire);
    if (deadline > 0 && steady_clock_ns() > deadline) {
        return 1;
    }
    return 0;
}

void DemuxThread::stop() {
    spdlog::info("[DemuxThread] stop() begin for {}", file_path_);
    running_.store(false);
    open_deadline_ns_.store(0, std::memory_order_release);
    abort_outputs();
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

bool DemuxThread::add_output(DemuxStreamKind kind, PacketQueue& output_queue) {
    if (running_.load()) {
        return false;
    }
    output_routes_.push_back(OutputRoute{kind, -1, &output_queue, false});
    return true;
}

bool DemuxThread::add_optional_output(DemuxStreamKind kind, PacketQueue& output_queue) {
    if (running_.load()) {
        return false;
    }
    output_routes_.push_back(OutputRoute{kind, -1, &output_queue, true});
    return true;
}

void DemuxThread::set_seek_callback(SeekCallback cb) {
    seek_callback_ = std::move(cb);
}

void DemuxThread::abort_outputs() {
    for (auto& route : output_routes_) {
        if (route.queue) {
            route.queue->abort();
        }
    }
}

void DemuxThread::flush_outputs() {
    for (auto& route : output_routes_) {
        if (route.queue) {
            route.queue->flush();
        }
    }
}

void DemuxThread::signal_outputs_eof() {
    for (auto& route : output_routes_) {
        if (route.queue) {
            route.queue->signal_eof();
        }
    }
}

int DemuxThread::stream_index_for_kind(DemuxStreamKind kind) const {
    switch (kind) {
    case DemuxStreamKind::Video:
        return stats_.video_stream_index;
    case DemuxStreamKind::Audio:
        return stats_.audio_stream_index;
    }
    return -1;
}

AVRational DemuxThread::time_base_for_stream(int stream_index) const {
    if (!fmt_ctx_ || stream_index < 0 ||
        stream_index >= static_cast<int>(fmt_ctx_->nb_streams)) {
        return AVRational{0, 1};
    }
    return fmt_ctx_->streams[stream_index]->time_base;
}

void DemuxThread::run() {
    spdlog::info("[DemuxThread] Demux loop started");

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        spdlog::error("[DemuxThread] Failed to allocate packet");
        running_.store(false);
        return;
    }

    int seek_stream_idx = stats_.video_stream_index >= 0
        ? stats_.video_stream_index
        : stats_.audio_stream_index;
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
                         output_routes_.empty() || !output_routes_[0].queue
                             ? 0
                             : output_routes_[0].queue->size());

            flush_outputs();

            int64_t target_tb = av_rescale_q(
                req.target_pts_us,
                {1, 1000000},
                time_base_for_stream(seek_stream_idx));
            int seek_ret = av_seek_frame(fmt_ctx_, seek_stream_idx, target_tb, AVSEEK_FLAG_BACKWARD);
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
                signal_outputs_eof();
                eof_reached = true;
                continue;
            }
            spdlog::error("[DemuxThread] Read error: {:#x}", static_cast<unsigned>(ret));
            break;
        }

        bool delivered = false;
        for (auto& route : output_routes_) {
            if (!route.queue || pkt->stream_index != route.stream_index) {
                continue;
            }

            // NOTE: Do NOT convert PTS here — keep packets in stream time_base.
            // The decode thread will convert frame PTS to microseconds after decoding.
            // Double-conversion would produce wildly incorrect timestamps.
            AVPacket* out = av_packet_clone(pkt);
            if (!out) {
                spdlog::error("[DemuxThread] Failed to clone packet");
                continue;
            }

            const bool pushed = route.optional
                ? route.queue->try_push(out)
                : route.queue->push(out);
            if (!pushed) {
                av_packet_free(&out);
                // Queue aborted or full — don't permanently exit, just drop this packet
                continue;
            }
            delivered = true;
        }
        av_packet_unref(pkt);

        if (delivered) {
            ++packets_pushed;
        }
    }

    av_packet_free(&pkt);
    // Signal decode thread that no more packets will come
    abort_outputs();
    spdlog::info("[DemuxThread] Demux loop ended");
}

} // namespace vr
