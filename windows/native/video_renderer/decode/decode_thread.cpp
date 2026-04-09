#include "video_renderer/decode/decode_thread.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace vr {

// get_format callback for hardware decode negotiation.
// Reads the preferred hw pixel format from opaque pointer (per-instance).
static enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                         const enum AVPixelFormat* pix_fmts) {
    auto* preferred = static_cast<AVPixelFormat*>(ctx->opaque);
    // Prefer D3D11VA_VLD over D3D11 when both are available.
    AVPixelFormat fallback = AV_PIX_FMT_NONE;
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (preferred && *p == *preferred) {
            return *p;
        }
        if (*p == AV_PIX_FMT_D3D11VA_VLD) {
            fallback = *p;
        }
    }
    if (fallback != AV_PIX_FMT_NONE) {
        spdlog::info("[DecodeThread] get_format: using D3D11VA_VLD fallback");
        return fallback;
    }
    spdlog::warn("[DecodeThread] HW pixel format not available in get_format, returning NONE");
    return AV_PIX_FMT_NONE;
}

DecodeThread::DecodeThread(PacketQueue& input_queue, TrackBuffer& output_buffer,
                           const AVCodecParameters* codec_params, AVRational time_base)
    : input_queue_(input_queue)
    , output_buffer_(output_buffer)
    , codec_params_(codec_params)
    , time_base_(time_base)
{
    codec_ = avcodec_find_decoder(codec_params->codec_id);
    if (!codec_) {
        spdlog::error("[DecodeThread] No decoder found for codec_id={}",
                      static_cast<int>(codec_params->codec_id));
        return;
    }

    spdlog::info("[DecodeThread] Using decoder: {}", codec_->name);

    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_) {
        spdlog::error("[DecodeThread] Failed to allocate codec context");
        return;
    }

    int ret = avcodec_parameters_to_context(codec_ctx_, codec_params);
    if (ret < 0) {
        spdlog::error("[DecodeThread] Failed to copy codec parameters: {:#x}", static_cast<unsigned>(ret));
        avcodec_free_context(&codec_ctx_);
        return;
    }

    // NOTE: avcodec_open2 is NOT called here.
    // It is deferred to start() so that enable_hardware_decode() can set
    // hw_device_ctx before the codec is opened.
}

DecodeThread::~DecodeThread() {
    stop();
}

bool DecodeThread::enable_hardware_decode(void* native_device,
                                           std::recursive_mutex* device_mutex) {
    if (!codec_ctx_ || !codec_) {
        spdlog::warn("[DecodeThread] Cannot enable hw decode: codec not initialized");
        return false;
    }

    native_device_ = native_device;
    device_mutex_ = device_mutex;

    auto result = try_hw_decode_providers(
        native_device, codec_, codec_params_->width, codec_params_->height,
        device_mutex);

    if (!result.success) {
        spdlog::info("[DecodeThread] Hardware decode not available, will use software");
        hw_enabled_ = false;
        return false;
    }

    // Store hw device context and provider
    hw_device_ctx_ = result.hw_device_ctx;
    hw_provider_ = std::move(result.provider);  // Must outlive hw_device_ctx (owns D3D11 mutex + context)
    hw_type_ = result.type;
    hw_enabled_ = true;
    hw_pix_fmt_ = result.hw_pix_fmt;

    // Set hw_device_ctx on codec context BEFORE opening
    codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);

    // Increase the hw frame pool size to accommodate both the decoder's
    // reference frame pool (DPB) and our preroll/render pipeline buffers.
    // Without this, the default pool (20) is too small when preroll frames
    // hold references via av_frame_ref while the decoder also needs surfaces.
    codec_ctx_->extra_hw_frames = 48;

    // NOTE: We intentionally do NOT create hw_frames_ctx here.
    // FFmpeg's internal ff_decode_get_hw_frames_ctx() will create one
    // automatically when the codec is opened with hw_device_ctx set.
    // This avoids configuration mismatches that caused 0-frame output.

    // Set get_format callback with per-instance opaque pointer
    codec_ctx_->get_format = get_hw_format;
    codec_ctx_->opaque = &hw_pix_fmt_;

    spdlog::info("[DecodeThread] Hardware decode enabled via {} (pix_fmt={})",
                 result.type == HwDecodeType::D3D11VA ? "D3D11VA" : "unknown",
                 static_cast<int>(result.hw_pix_fmt));
    return true;
}

bool DecodeThread::open_codec() {
    int ret = avcodec_open2(codec_ctx_, codec_, nullptr);
    if (ret == 0) return true;

    spdlog::error("[DecodeThread] Failed to open codec: {:#x}", static_cast<unsigned>(ret));

    // If HW decode was attempted, try fallback to software
    if (hw_enabled_) {
        spdlog::info("[DecodeThread] Attempting software fallback...");

        // Clean up hw state
        if (codec_ctx_->hw_device_ctx) {
            av_buffer_unref(&codec_ctx_->hw_device_ctx);
        }
        hw_enabled_ = false;
        hw_pix_fmt_ = AV_PIX_FMT_NONE;

        // Recreate codec context without hw
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = avcodec_alloc_context3(codec_);
        if (!codec_ctx_) {
            spdlog::error("[DecodeThread] Failed to allocate sw codec context");
            return false;
        }

        int ret2 = avcodec_parameters_to_context(codec_ctx_, codec_params_);
        if (ret2 < 0) {
            spdlog::error("[DecodeThread] Failed to copy params for sw fallback");
            avcodec_free_context(&codec_ctx_);
            return false;
        }

        ret2 = avcodec_open2(codec_ctx_, codec_, nullptr);
        if (ret2 < 0) {
            spdlog::error("[DecodeThread] Software fallback also failed: {:#x}", static_cast<unsigned>(ret2));
            return false;
        }

        spdlog::info("[DecodeThread] Software fallback succeeded");
        return true;
    }

    return false;
}

bool DecodeThread::start() {
    if (!codec_ctx_) {
        spdlog::error("[DecodeThread] Cannot start: codec not initialized");
        return false;
    }
    if (running_.load()) return false;

    // Open codec (deferred from constructor so hw_device_ctx can be set first)
    if (!open_codec()) {
        spdlog::error("[DecodeThread] Cannot start: codec open failed");
        return false;
    }

    spdlog::info("[DecodeThread] Codec opened successfully ({}x{})",
                 codec_ctx_->width, codec_ctx_->height);

    // Initialize the frame converter based on decode mode
    bool conv_ok;
    if (hw_enabled_) {
        conv_ok = converter_.init_hardware(native_device_, nullptr,
                                           codec_ctx_->width, codec_ctx_->height,
                                           hw_type_);
    } else {
        conv_ok = converter_.init_software(codec_ctx_->width, codec_ctx_->height,
                                           codec_ctx_->pix_fmt);
    }
    if (!conv_ok) {
        spdlog::error("[DecodeThread] Failed to initialize frame converter");
        return false;
    }

    output_buffer_.set_state(TrackState::Buffering);
    running_.store(true);
    thread_ = std::thread(&DecodeThread::run, this);
    return true;
}

void DecodeThread::stop() {
    running_.store(false);
    input_queue_.abort();   // Unblock blocking pop
    output_buffer_.abort(); // Unblock blocking push_frame
    if (thread_.joinable()) {
        thread_.join();
    }
    // Release output frames BEFORE freeing hw resources.
    // TextureFrames hold hw_frame_ref (av_frame_ref) which reference
    // hw_frames_ctx -> hw_device_ctx. If hw_device_ctx is freed first,
    // the frame cleanup will access a freed device context (SIGSEGV).
    output_buffer_.clear_frames();

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
    hw_provider_.reset();
}

void DecodeThread::set_decode_paused(bool paused) {
    decode_paused_.store(paused, std::memory_order_release);
}

void DecodeThread::notify_seek(int64_t target_pts_us, SeekType type) {
    cancelled_.store(true, std::memory_order_release);  // Abort in-progress decode
    std::lock_guard<std::mutex> lock(seek_mutex_);
    seek_.target_pts_us = target_pts_us;
    seek_.type = type;
    seek_.pending = true;
}

void DecodeThread::drain_codec(AVFrame* frame, const std::function<void(AVFrame*)>& rescale_ts, int64_t target_us) {
    auto prev_level = av_log_get_level();
    av_log_set_level(AV_LOG_ERROR);
    avcodec_send_packet(codec_ctx_, nullptr);
    while (avcodec_receive_frame(codec_ctx_, frame) >= 0) {
        if (cancelled_.load(std::memory_order_acquire)) {
            av_frame_unref(frame);
            break;
        }
        if (target_us >= 0) {
            rescale_ts(frame);
            TextureFrame tex_frame = converter_.convert(frame);
            if (tex_frame.pts_us >= target_us) {
                exact_seek_reorder_.push_back(std::move(tex_frame));
            }
        }
        av_frame_unref(frame);
    }
    avcodec_flush_buffers(codec_ctx_);
    av_log_set_level(prev_level);
    eof_flushed_ = true;
}

void DecodeThread::flush_reorder_buffer() {
    if (exact_seek_reorder_.empty()) return;
    std::sort(exact_seek_reorder_.begin(), exact_seek_reorder_.end(),
              [](const TextureFrame& a, const TextureFrame& b) {
                  return a.pts_us < b.pts_us;
              });
    for (auto& f : exact_seek_reorder_) {
        output_buffer_.push_frame(std::move(f));
    }
    spdlog::info("[DecodeThread] Exact seek reorder: {} frames sorted and pushed",
                 exact_seek_reorder_.size());
    exact_seek_reorder_.clear();
    exact_seek_target_us_ = -1;
}

void DecodeThread::run() {
    spdlog::info("[DecodeThread] Decode loop started (hw={})", hw_enabled_);

    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        spdlog::error("[DecodeThread] Failed to allocate frame");
        output_buffer_.set_state(TrackState::Error);
        return;
    }

    // Rescale frame timestamps from stream time_base to microseconds
    auto rescale_ts = [&](AVFrame* f) {
        if (f->pts != AV_NOPTS_VALUE) {
            f->pts = av_rescale_q(f->pts, time_base_, {1, 1000000});
        } else if (f->best_effort_timestamp != AV_NOPTS_VALUE) {
            f->pts = av_rescale_q(f->best_effort_timestamp, time_base_, {1, 1000000});
        }
        if (f->duration > 0 && f->duration != AV_NOPTS_VALUE) {
            f->duration = av_rescale_q(f->duration, time_base_, {1, 1000000});
        }
    };

    while (running_.load()) {
        // Handle seek notification — atomically take the pending seek
        int64_t target_us = -1;
        SeekType seek_type = SeekType::Keyframe;
        {
            std::lock_guard<std::mutex> lock(seek_mutex_);
            if (seek_.pending) {
                seek_.pending = false;
                target_us = seek_.target_pts_us;
                seek_type = seek_.type;
            }
        }
        if (target_us >= 0) {
            // Reset cancellation flag — this seek is now the active operation
            cancelled_.store(false, std::memory_order_release);

            spdlog::info("[DecodeThread] === SEEK START: target={:.3f}s, type={}, "
                         "input_pq={}, output_buf={}, buf_state={}",
                         target_us / 1e6,
                         seek_type == SeekType::Exact ? "Exact" : "Keyframe",
                         input_queue_.size(),
                         output_buffer_.total_count(),
                         static_cast<int>(output_buffer_.state()));

            // Do NOT flush the hardware codec.
            // avcodec_flush_buffers releases D3D11VA decoder surfaces back to the
            // pool, but the D3D11VA internal state can become inconsistent after
            // flush — the next avcodec_send_packet crashes inside D3D11 with a
            // C++ exception.  This is a known issue with D3D11VA: the driver-level
            // video decoder object does not survive flush cleanly.
            //
            // Instead, simply skip flush and let the first keyframe from the seek
            // position reset the DPB.  The DemuxThread has already:
            //   1) flushed the packet queue (no stale packets)
            //   2) seeked to a keyframe via av_seek_frame(AVSEEK_FLAG_BACKWARD)
            // So the first packet sent to the codec will be a keyframe (typically
            // IDR for H.264/HEVC), which resets the decoder state.
            //
            // For software decode, flush is safe and cheap — it just resets the
            // DPB without touching any hardware state.
            if (!hw_enabled_) {
                avcodec_flush_buffers(codec_ctx_);
            }
            spdlog::info("[DecodeThread] Seek flush: {}",
                         hw_enabled_ ? "SKIPPED (D3D11VA unsafe)" : "codec buffers flushed");

            // NOTE: Do NOT drain input queue here! The DemuxThread already
            // flushes the queue before seeking and then pushes NEW packets.
            // Draining here would discard those fresh post-seek packets.

            // Set up exact seek frame discard if needed
            if (seek_type == SeekType::Exact) {
                exact_seek_target_us_ = target_us;
                spdlog::info("[DecodeThread] Exact seek: will discard frames < {:.3f}s", target_us / 1e6);
            } else {
                exact_seek_target_us_ = -1;
            }

            // Fast preroll after seek: only need 1 frame to resume display
            post_seek_ = true;

            eof_flushed_ = false;
            decode_paused_.store(false, std::memory_order_release);
            output_buffer_.set_state(TrackState::Buffering);
            spdlog::info("[DecodeThread] === SEEK DONE: state->Buffering, post_seek fast preroll, waiting for new packets");
            continue;
        }

        // Non-blocking pop with short sleep — allows seek_pending to be checked promptly
        AVPacket* pkt = input_queue_.try_pop();
        if (!pkt) {
            // EOF flush: drain codec once when the producer signals EOF.
            // Skip during Buffering (post-seek preroll) — the DemuxThread may
            // signal EOF very quickly after seek (file is cached), but we need
            // to keep decoding to fill the preroll buffer first.
            if (input_queue_.is_eof() && !eof_flushed_) {
                if (output_buffer_.state() == TrackState::Buffering) {
                    // Drain codec for exact seek to flush remaining DPB frames
                    if (exact_seek_target_us_ >= 0) {
                        drain_codec(frame, rescale_ts, exact_seek_target_us_);
                        spdlog::info("[DecodeThread] Exact seek EOF drain: reorder buffer has {} frames",
                                     exact_seek_reorder_.size());
                    } else {
                        eof_flushed_ = true;
                    }

                    // Flush exact-seek reorder buffer at EOF — no more frames coming.
                    flush_reorder_buffer();
                    // Preroll check — may complete if reorder flush added frames
                    if (post_seek_ && output_buffer_.total_count() >= 1) {
                        spdlog::info("[DecodeThread] === Preroll complete (EOF): {} frames, state->Ready",
                                     output_buffer_.total_count());
                        output_buffer_.set_state(TrackState::Ready);
                        post_seek_ = false;
                    } else {
                        spdlog::info("[DecodeThread] EOF seen during Buffering, deferring codec flush "
                                     "(buf={}, pq={})",
                                     output_buffer_.total_count(), input_queue_.size());
                    }
                } else {
                    avcodec_send_packet(codec_ctx_, nullptr);
                    while (true) {
                        int ret = avcodec_receive_frame(codec_ctx_, frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                        if (ret < 0) break;

                        rescale_ts(frame);
                        TextureFrame tex_frame = converter_.convert(frame);
                        output_buffer_.push_frame(std::move(tex_frame));
                        av_frame_unref(frame);
                    }
                    // Flush decode device after EOF drain to ensure shared NV12
                    // textures are visible to the render device.
                    if (hw_enabled_ && hw_provider_) {
                        hw_provider_->flush();
                    }
                    eof_flushed_ = true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        eof_flushed_ = false;

        // If decode is paused (seek transition), discard the packet without
        // sending to codec.  This prevents the HEVC decoder from emitting
        // "Could not find ref with POC" warnings on stale packets.
        {
            std::lock_guard<std::mutex> seek_lock(seek_mutex_);
            if (seek_.pending) {
                av_packet_free(&pkt);
                continue;
            }
        }
        if (decode_paused_.load(std::memory_order_acquire) ||
            output_buffer_.state() == TrackState::Flushing) {
            av_packet_free(&pkt);
            continue;
        }

        // Cancel checkpoint: abort if a new seek arrived while we were waiting
        if (cancelled_.load(std::memory_order_acquire)) {
            av_packet_free(&pkt);
            continue;
        }

        int ret = 0;
        auto batch_t0 = std::chrono::steady_clock::now();
        if (hw_enabled_ && device_mutex_) {
            std::lock_guard<std::recursive_mutex> d3d_lock(*device_mutex_);
            ret = avcodec_send_packet(codec_ctx_, pkt);
        } else {
            ret = avcodec_send_packet(codec_ctx_, pkt);
        }
        av_packet_free(&pkt);

        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            spdlog::error("[DecodeThread] Error sending packet: {:#x}", static_cast<unsigned>(ret));
            continue;
        }

        int frames_produced = 0;
        while (true) {
            if (cancelled_.load(std::memory_order_acquire)) break;
            if (hw_enabled_ && device_mutex_) {
                std::lock_guard<std::recursive_mutex> d3d_lock(*device_mutex_);
                ret = avcodec_receive_frame(codec_ctx_, frame);
            } else {
                ret = avcodec_receive_frame(codec_ctx_, frame);
            }
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                spdlog::error("[DecodeThread] Error receiving frame: {:#x}", static_cast<unsigned>(ret));
                break;
            }

            rescale_ts(frame);

            TextureFrame tex_frame = converter_.convert(frame);
            ++frames_produced;

            // Exact seek: discard frames before the target PTS
            if (exact_seek_target_us_ >= 0) {
                if (tex_frame.pts_us < exact_seek_target_us_) {
                    perf_.frames_dropped.fetch_add(1, std::memory_order_relaxed);
                    av_frame_unref(frame);
                    continue; // Discard intermediate frame
                }
                // Buffer frames for B-frame reordering instead of pushing
                // immediately — H265 B-frames may be output by the decoder
                // in non-PTS order (decode order != display order).
                exact_seek_reorder_.push_back(std::move(tex_frame));
                av_frame_unref(frame);
                continue;
            }

            output_buffer_.push_frame(std::move(tex_frame));

            // Flush the independent decode device after producing HW frames.
            // DXGI requires the producing device to call Flush() before the
            // consuming device (render device) can read shared texture data.
            // Without this, the render device may see incomplete NV12 data
            // (upper half content, lower half gray / green) on the first frame.
            if (hw_enabled_ && hw_provider_ && frames_produced == 1) {
                hw_provider_->flush();
            }

            if (output_buffer_.state() == TrackState::Buffering) {
                bool ready = post_seek_
                    ? output_buffer_.total_count() >= 1
                    : output_buffer_.has_preroll();
                if (ready) {
                    spdlog::info("[DecodeThread] === Preroll complete: {} frames buffered, post_seek={}, state->Ready",
                                 output_buffer_.total_count(), post_seek_);
                    output_buffer_.set_state(TrackState::Ready);
                    post_seek_ = false;
                }
            }

            av_frame_unref(frame);
        }

        // Exact seek B-frame reordering.
        // H265 outputs frames across MULTIPLE avcodec_send_packet calls due to
        // DPB reordering delay.  The first frame >= target might be a reference
        // frame (higher PTS), while B-frames with lower PTS arrive in subsequent
        // batches.  We accumulate frames and only flush when the lowest PTS in
        // the buffer is close enough to the target, or at EOF / max count.
        if (exact_seek_target_us_ >= 0 && !exact_seek_reorder_.empty()) {
            bool should_flush = false;

            if (input_queue_.is_eof() && input_queue_.size() == 0) {
                if (!eof_flushed_) {
                    drain_codec(frame, rescale_ts, exact_seek_target_us_);
                    spdlog::info("[DecodeThread] Exact seek EOF: codec drain, reorder buffer now has {} frames",
                                 exact_seek_reorder_.size());
                }
                should_flush = true;
            } else {
                // Check PTS gap: if the lowest PTS in the buffer is still far
                // from the target, there may be B-frames with lower PTS stuck
                // in the DPB that haven't been output yet.
                auto min_it = std::min_element(exact_seek_reorder_.begin(),
                    exact_seek_reorder_.end(),
                    [](const TextureFrame& a, const TextureFrame& b) {
                        return a.pts_us < b.pts_us;
                    });
                int64_t gap = min_it->pts_us - exact_seek_target_us_;

                // Flush when the closest frame is within ~1 frame duration of
                // target (gap < 17ms for 60fps means the right frame is likely
                // already in the buffer).  Otherwise keep waiting for B-frames
                // still in the DPB.  Safety cap at 16 frames.
                if (gap < 17000 || exact_seek_reorder_.size() >= 16) {
                    should_flush = true;
                }
            }

            if (should_flush) {
                flush_reorder_buffer();
                auto first = output_buffer_.peek(0);
                spdlog::info("[DecodeThread] Exact seek reorder: frames pushed, first_pts={:.3f}s",
                             first.has_value() ? first->pts_us / 1e6 : -1.0);
            }
        }

        // Preroll check
        if (output_buffer_.state() == TrackState::Buffering) {
            bool ready = post_seek_
                ? output_buffer_.total_count() >= 1
                : output_buffer_.has_preroll();
            if (ready) {
                spdlog::info("[DecodeThread] === Preroll complete: {} frames buffered, post_seek={}, state->Ready",
                             output_buffer_.total_count(), post_seek_);
                output_buffer_.set_state(TrackState::Ready);
                post_seek_ = false;
            }
        }

        if (frames_produced > 0) {
            uint64_t batch_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - batch_t0).count());
            perf_.frames_decoded.fetch_add(frames_produced, std::memory_order_relaxed);
            perf_.total_decode_us.fetch_add(batch_us, std::memory_order_relaxed);
            // Update peak (CAS loop)
            uint64_t cur_max = perf_.max_decode_us.load(std::memory_order_relaxed);
            while (batch_us > cur_max &&
                   !perf_.max_decode_us.compare_exchange_weak(cur_max, batch_us,
                                                              std::memory_order_relaxed)) {}
            spdlog::debug("[DecodeThread] Decoded {} frames in {}us, buf_state={}, buf_count={}",
                          frames_produced, batch_us, static_cast<int>(output_buffer_.state()),
                          output_buffer_.total_count());
        }
    }

    output_buffer_.set_state(TrackState::Flushing);
    av_frame_free(&frame);
    spdlog::info("[DecodeThread] Decode loop ended");
}

} // namespace vr
