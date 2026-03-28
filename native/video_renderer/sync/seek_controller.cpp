#include "video_renderer/sync/seek_controller.h"
#include <spdlog/spdlog.h>

namespace vr {

SeekController::SeekController()
{}

void SeekController::request_seek(int64_t target_pts_us, SeekType type) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.target_pts_us = target_pts_us;
    pending_.type = type;
    has_pending_.store(true, std::memory_order_release);
    spdlog::debug("[SeekController] Seek requested: target={} us, type={}",
                  target_pts_us, static_cast<int>(type));
}

bool SeekController::has_pending_seek() const {
    return has_pending_.load(std::memory_order_acquire);
}

SeekRequest SeekController::pending_request() const {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    return pending_;
}

void SeekController::clear_pending() {
    has_pending_.store(false, std::memory_order_release);
}

bool SeekController::execute_seek(AVFormatContext* fmt_ctx, int stream_index,
                                  const AVRational& time_base,
                                  AVCodecContext* codec_ctx) {
    if (!has_pending_.load(std::memory_order_acquire)) {
        return false;
    }

    SeekRequest req;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        req = pending_;
        has_pending_.store(false, std::memory_order_release);
    }

    bool result = false;
    switch (req.type) {
    case SeekType::Keyframe:
        if (codec_ctx) avcodec_flush_buffers(codec_ctx);
        result = seek_keyframe(fmt_ctx, stream_index, time_base, req.target_pts_us);
        break;
    case SeekType::Exact:
        result = seek_exact(fmt_ctx, stream_index, time_base, codec_ctx, req.target_pts_us);
        break;
    }

    if (result) {
        spdlog::info("[SeekController] Seek succeeded: target={} us, type={}",
                     req.target_pts_us, static_cast<int>(req.type));
    } else {
        spdlog::warn("[SeekController] Seek failed: target={} us, type={}",
                     req.target_pts_us, static_cast<int>(req.type));
    }

    return result;
}

bool SeekController::seek_keyframe(AVFormatContext* fmt_ctx, int stream_index,
                                   const AVRational& time_base, int64_t target_pts_us) {
    // Convert from microseconds to stream time_base
    int64_t target_tb = av_rescale_q(target_pts_us, {1, 1000000}, time_base);

    spdlog::debug("[SeekController] Keyframe seek: target_tb={}", target_tb);

    int ret = av_seek_frame(fmt_ctx, stream_index, target_tb, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        spdlog::error("[SeekController] av_seek_frame failed: {}", ret);
        return false;
    }

    return true;
}

bool SeekController::seek_exact(AVFormatContext* fmt_ctx, int stream_index,
                                const AVRational& time_base, AVCodecContext* codec_ctx,
                                int64_t target_pts_us) {
    if (!codec_ctx) {
        spdlog::error("[SeekController] Exact seek requires codec_ctx");
        return false;
    }

    // Flush the codec before seeking
    avcodec_flush_buffers(codec_ctx);

    // Seek to the I-frame before the target
    int64_t target_tb = av_rescale_q(target_pts_us, {1, 1000000}, time_base);

    spdlog::debug("[SeekController] Exact seek: target_tb={}", target_tb);

    int ret = av_seek_frame(fmt_ctx, stream_index, target_tb, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        spdlog::error("[SeekController] av_seek_frame failed: {}", ret);
        return false;
    }

    // Decode forward to the target PTS, discarding frames before it
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        spdlog::error("[SeekController] Failed to allocate frame");
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        av_frame_free(&frame);
        spdlog::error("[SeekController] Failed to allocate packet");
        return false;
    }

    bool found = false;
    const int max_attempts = 500; // Safety limit to prevent infinite loop

    for (int attempt = 0; attempt < max_attempts && !found; ++attempt) {
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) {
            // EOF or error
            break;
        }

        if (pkt->stream_index != stream_index) {
            av_packet_unref(pkt);
            continue;
        }

        // Send packet to decoder
        ret = avcodec_send_packet(codec_ctx, pkt);
        av_packet_unref(pkt);

        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            continue;
        }

        // Receive frames
        while (true) {
            ret = avcodec_receive_frame(codec_ctx, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                break;
            }

            int64_t frame_pts_us = av_rescale_q(frame->pts, time_base, {1, 1000000});
            if (frame_pts_us >= target_pts_us) {
                found = true;
                av_frame_unref(frame);
                break;
            }

            av_frame_unref(frame);
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);

    if (!found) {
        spdlog::warn("[SeekController] Exact seek did not find target frame");
    }

    return true; // Position is at or near target even if exact frame not found
}

} // namespace vr
