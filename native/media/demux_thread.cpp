#include "media/demux_thread.h"

#include "media/source_packet_identity.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace vr {

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
    if (!open()) {
        return false;
    }
    if (!start_thread()) {
        stop();
        return false;
    }
    return true;
}

bool DemuxThread::open() {
    auto fail_open = [this]() {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        running_.store(false, std::memory_order_release);
        input_.close();
        opening_ = false;
        lock.unlock();
        lifecycle_cv_.notify_all();
        return false;
    };

    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (running_.load(std::memory_order_acquire) || opening_ ||
            thread_.joinable() || input_.is_open()) {
            return false;
        }

        stats_ = DemuxStats{};
        for (auto& route : output_routes_) {
            route.stream_index = -1;
        }
        opening_ = true;
        running_.store(true, std::memory_order_release);
    }

    MediaInputOpenOptions input_options;
    input_options.interrupt_requested = [this]() {
        return !running_.load(std::memory_order_acquire);
    };
    std::string input_error;
    if (!input_.open(file_path_, input_options, input_error)) {
        spdlog::error(
            "[DemuxThread] Failed to open input {}: {}",
            file_path_,
            input_error);
        return fail_open();
    }
    stats_ = input_.stats();
    if (!running_.load(std::memory_order_acquire)) {
        spdlog::info(
            "[DemuxThread] Open cancelled during probe: {}",
            file_path_);
        return fail_open();
    }
    if (input_.uses_private_demuxer()) {
        spdlog::info(
            "[DemuxThread] Using private CDN FLV demuxer for {}",
            file_path_);
    }

    if (output_routes_.empty()) {
        spdlog::error("[DemuxThread] No output routes registered for {}", file_path_);
        return fail_open();
    }

    for (auto& route : output_routes_) {
        route.stream_index = stream_index_for_kind(route.kind);
        if (route.stream_index < 0) {
            if (route.optional) {
                continue;
            }
            spdlog::error("[DemuxThread] Requested output stream is missing in {}", file_path_);
            return fail_open();
        }
    }

    spdlog::info("[DemuxThread] Opened {} ({}x{}, stream={}, tb={}/{})",
                 file_path_, stats_.width, stats_.height,
                 stats_.video_stream_index,
                 stats_.time_base.num, stats_.time_base.den);

    {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        if (!running_.load(std::memory_order_acquire)) {
            spdlog::info("[DemuxThread] Open cancelled before demux thread start: {}", file_path_);
            input_.close();
            opening_ = false;
            lock.unlock();
            lifecycle_cv_.notify_all();
            return false;
        }
        opening_ = false;
    }
    lifecycle_cv_.notify_all();
    return true;
}

bool DemuxThread::start_thread() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (opening_ || !running_.load(std::memory_order_acquire) ||
        thread_.joinable() || !input_.is_open()) {
        return false;
    }
    thread_ = std::thread(&DemuxThread::run, this);
    return true;
}

void DemuxThread::stop() {
    spdlog::info("[DemuxThread] stop() begin for {}", file_path_);
    {
        std::unique_lock<std::mutex> lock(lifecycle_mutex_);
        running_.store(false, std::memory_order_release);
        abort_outputs();
        lifecycle_cv_.wait(lock, [this] { return !opening_; });
    }
    if (thread_.joinable()) {
        spdlog::info("[DemuxThread] stop() waiting for join: {}", file_path_);
        thread_.join();
        spdlog::info("[DemuxThread] stop() joined: {}", file_path_);
    }
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (input_.is_open()) {
            spdlog::info("[DemuxThread] stop() closing input: {}", file_path_);
            input_.close();
        }
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
    std::lock_guard<std::mutex> lock(seek_callback_mutex_);
    seek_callback_ = std::move(cb);
}

void DemuxThread::set_error_callback(ErrorCallback cb) {
    std::lock_guard<std::mutex> lock(error_callback_mutex_);
    error_callback_ = std::move(cb);
}

void DemuxThread::fail_next_read_for_test(int error_code) {
    forced_read_error_for_test_.store(error_code, std::memory_order_release);
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

void DemuxThread::emit_error(int error_code) {
    ErrorCallback error_callback;
    {
        std::lock_guard<std::mutex> lock(error_callback_mutex_);
        error_callback = error_callback_;
    }
    if (error_callback) {
        error_callback(error_code);
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
    return input_.time_base_for_stream(stream_index);
}

void DemuxThread::run() {
    spdlog::info("[DemuxThread] Demux loop started");

    auto packet = AvPacketOwner::allocate();
    if (!packet) {
        spdlog::error("[DemuxThread] Failed to allocate packet");
        running_.store(false, std::memory_order_release);
        abort_outputs();
        return;
    }
    AVPacket* pkt = packet.get();

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
                         is_exact_seek_type(req.type) ? "Exact" : "Keyframe",
                         output_routes_.empty() || !output_routes_[0].queue
                             ? 0
                             : output_routes_[0].queue->size());

            flush_outputs();

            int64_t target_tb = av_rescale_q(
                req.target_pts_us,
                {1, 1000000},
                time_base_for_stream(seek_stream_idx));
            int seek_ret = input_.seek(
                seek_stream_idx,
                target_tb,
                AVSEEK_FLAG_BACKWARD);
            if (seek_ret < 0) {
                spdlog::error("[DemuxThread] av_seek_frame FAILED: target={:.3f}s, ret={:#x}",
                             req.target_pts_us / 1e6, static_cast<unsigned>(seek_ret));
            } else {
                // Clear demuxer EOF/read-ahead state so av_read_frame() starts
                // producing packets again after seeks from end-of-file.
                input_.flush();
                spdlog::info("[DemuxThread] av_seek_frame OK: target={:.3f}s", req.target_pts_us / 1e6);
            }

            SeekCallback seek_callback;
            {
                std::lock_guard<std::mutex> lock(seek_callback_mutex_);
                seek_callback = seek_callback_;
            }
            if (seek_callback) {
                spdlog::info("[DemuxThread] Invoking seek callback -> DecodeThread");
                seek_callback(req.target_pts_us, req.type);
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

        const int forced_read_error =
            forced_read_error_for_test_.exchange(0, std::memory_order_acq_rel);
        int ret = forced_read_error != 0
            ? forced_read_error
            : input_.read_packet(pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                spdlog::info("[DemuxThread] EOF reached after {} packets, waiting for seek",
                             packets_pushed);
                signal_outputs_eof();
                eof_reached = true;
                continue;
            }
            spdlog::error("[DemuxThread] Read error: {:#x}", static_cast<unsigned>(ret));
            emit_error(ret);
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
            if (pkt->stream_index == stats_.video_stream_index && !pkt->opaque_ref) {
                SourcePacketIdentity identity;
                identity.stream_index = pkt->stream_index;
                identity.packet_index = next_video_packet_identity_index_++;
                identity.position = pkt->pos;
                identity.pts = pkt->pts;
                identity.dts = pkt->dts;
                identity.duration = pkt->duration;
                identity.size = pkt->size;
                identity.flags = pkt->flags;
                attach_source_packet_identity(pkt, identity);
            }

            auto out = AvPacketOwner::clone(pkt);
            if (!out) {
                spdlog::error("[DemuxThread] Failed to clone packet");
                continue;
            }

            const bool pushed = route.optional
                ? route.queue->try_push(out.get())
                : route.queue->push(out.get());
            if (!pushed) {
                // Queue aborted or full — don't permanently exit, just drop this packet
                continue;
            }
            out.release();
            delivered = true;
        }
        av_packet_unref(pkt);

        if (delivered) {
            ++packets_pushed;
        }
    }

    // Signal decode thread that no more packets will come
    abort_outputs();
    spdlog::info("[DemuxThread] Demux loop ended");
}

} // namespace vr
