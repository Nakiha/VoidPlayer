#include "video_renderer/decode/decode_thread.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace vr {

namespace {
constexpr int64_t kExactSeekLookbehindUs = 500000;  // Keep up to 0.5s before target.
constexpr int64_t kExactSeekLeadUs = 250000;        // HEVC B-frames can appear long after refs.
constexpr size_t kExactSeekReorderPublishFrames = 64;
constexpr size_t kExactSeekPreviewWindowFrames = 4;

size_t post_seek_preroll_target(bool hw_enabled) {
    // Hardware-decoded seek/add-track previews are more stable if we wait for
    // one extra frame before exposing the paused preview. Some GPU/driver
    // combinations can produce a partially-ready first post-seek frame.
    return hw_enabled ? size_t(2) : size_t(1);
}

bool log_codec_exception(const char* stage, AVCodecID codec_id, bool hw_enabled) {
    spdlog::error("[DecodeThread] Unhandled exception during {} (codec_id={}, hw={})",
                  stage, static_cast<int>(codec_id), hw_enabled);
    return false;
}
}  // namespace

// get_format callback for hardware decode negotiation.
// Reads the preferred hw pixel format from opaque pointer (per-instance).
static enum AVPixelFormat get_hw_format(AVCodecContext* ctx,
                                         const enum AVPixelFormat* pix_fmts) {
    auto* preferred = static_cast<AVPixelFormat*>(ctx->opaque);
    AVPixelFormat fallback = AV_PIX_FMT_NONE;
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (preferred && *p == *preferred) {
            return *p;
        }
        if (*p == AV_PIX_FMT_D3D11) {
            fallback = *p;
        }
    }
    if (fallback != AV_PIX_FMT_NONE) {
        spdlog::info("[DecodeThread] get_format: using D3D11 fallback");
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
    codec_ = nullptr;
    if (codec_params->codec_id == AV_CODEC_ID_AV1) {
        codec_ = avcodec_find_decoder_by_name("av1");
        if (codec_) {
            spdlog::info("[DecodeThread] AV1: using native decoder first for hardware negotiation "
                         "(libdav1d remains software fallback)");
        }
    }
    if (!codec_) {
        codec_ = avcodec_find_decoder(codec_params->codec_id);
    }
    if (!codec_) {
        spdlog::error("[DecodeThread] No decoder found for codec_id={}",
                      static_cast<int>(codec_params->codec_id));
        return;
    }

    spdlog::info("[DecodeThread] Using decoder: {}", codec_->name);

    if (!reset_codec_context(codec_)) {
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
        const AVCodec* sw_codec = preferred_software_decoder();
        if (sw_codec && sw_codec != codec_) {
            spdlog::info("[DecodeThread] Switching decoder to {} for software fallback", sw_codec->name);
            reset_codec_context(sw_codec);
        }
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

    // Increase the hw frame pool only when decoded hardware surfaces can be
    // held by the render queue. Hwdownload paths release decoder surfaces
    // immediately after transfer; forcing a large D3D11VA pool there is both
    // unnecessary and can produce black transfer frames on some AV1 drivers.
    if (hardware_surfaces_are_renderer_owned()) {
        codec_ctx_->extra_hw_frames = 48;
    }

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
    if (!hw_enabled_) {
        const AVCodec* sw_codec = preferred_software_decoder();
        if (sw_codec && sw_codec != codec_) {
            spdlog::info("[DecodeThread] Using decoder: {} (software fallback)", sw_codec->name);
            if (!reset_codec_context(sw_codec)) {
                return false;
            }
        }
    }

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

        const AVCodec* sw_codec = preferred_software_decoder();
        if (sw_codec && sw_codec != codec_) {
            spdlog::info("[DecodeThread] Switching decoder to {} for software fallback", sw_codec->name);
        }
        if (!reset_codec_context(sw_codec ? sw_codec : codec_)) {
            return false;
        }

        int ret2 = avcodec_open2(codec_ctx_, codec_, nullptr);
        if (ret2 < 0) {
            spdlog::error("[DecodeThread] Software fallback also failed: {:#x}", static_cast<unsigned>(ret2));
            return false;
        }

        spdlog::info("[DecodeThread] Software fallback succeeded");
        return true;
    }

    return false;
}

bool DecodeThread::reset_codec_context(const AVCodec* codec) {
    if (!codec) {
        spdlog::error("[DecodeThread] Cannot allocate codec context: decoder is null");
        return false;
    }

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }

    codec_ = codec;
    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_) {
        spdlog::error("[DecodeThread] Failed to allocate codec context for {}", codec_->name);
        return false;
    }

    int ret = avcodec_parameters_to_context(codec_ctx_, codec_params_);
    if (ret < 0) {
        spdlog::error("[DecodeThread] Failed to copy codec parameters for {}: {:#x}",
                      codec_->name, static_cast<unsigned>(ret));
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    return true;
}

const AVCodec* DecodeThread::preferred_software_decoder() const {
    if (!codec_params_) {
        return codec_;
    }

    if (codec_params_->codec_id == AV_CODEC_ID_AV1) {
        const AVCodec* dav1d = avcodec_find_decoder_by_name("libdav1d");
        if (dav1d) {
            return dav1d;
        }
    }

    return codec_;
}

bool DecodeThread::hardware_output_downloads_to_cpu() const {
    if (!codec_params_) {
        return false;
    }
    return codec_params_->codec_id == AV_CODEC_ID_AV1 ||
           codec_params_->codec_id == AV_CODEC_ID_VP9;
}

bool DecodeThread::hardware_surfaces_are_renderer_owned() const {
    return hw_enabled_ && !hardware_output_downloads_to_cpu();
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
                                           hw_type_,
                                           hardware_output_downloads_to_cpu());
    } else {
        conv_ok = converter_.init_software(codec_ctx_->width, codec_ctx_->height,
                                           codec_ctx_->pix_fmt);
    }
    if (!conv_ok) {
        spdlog::error("[DecodeThread] Failed to initialize frame converter");
        return false;
    }

    output_buffer_.set_state(TrackState::Buffering);
    hw_visibility_flush_pending_ = hw_enabled_;
    running_.store(true);
    thread_ = std::thread(&DecodeThread::run, this);
    return true;
}

void DecodeThread::stop() {
    spdlog::info("[DecodeThread] stop() begin");
    running_.store(false);
    cancelled_.store(true, std::memory_order_release);
    input_queue_.clear_eof();
    input_queue_.abort();   // Unblock blocking pop
    output_buffer_.abort(); // Unblock blocking push_frame
    if (thread_.joinable()) {
        spdlog::info("[DecodeThread] stop() waiting for decode thread join");
        thread_.join();
        spdlog::info("[DecodeThread] stop() decode thread joined");
    }
    // Release output frames BEFORE freeing hw resources.
    // TextureFrames hold hw_frame_ref (av_frame_ref) which reference
    // hw_frames_ctx -> hw_device_ctx. If hw_device_ctx is freed first,
    // the frame cleanup will access a freed device context (SIGSEGV).
    spdlog::info("[DecodeThread] stop() clearing output frames");
    output_buffer_.clear_frames();
    spdlog::info("[DecodeThread] stop() output frames cleared");

    if (codec_ctx_) {
        spdlog::info("[DecodeThread] stop() freeing codec context");
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (hw_device_ctx_) {
        spdlog::info("[DecodeThread] stop() releasing hw device context");
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
    hw_provider_.reset();
    spdlog::info("[DecodeThread] stop() end");
}

void DecodeThread::set_decode_paused(bool paused) {
    decode_paused_.store(paused, std::memory_order_release);
}

void DecodeThread::set_pause_after_preroll(bool enabled) {
    pause_after_preroll_.store(enabled, std::memory_order_release);
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
    try {
        avcodec_send_packet(codec_ctx_, nullptr);
    } catch (...) {
        output_buffer_.set_state(TrackState::Error);
        running_.store(false, std::memory_order_release);
        log_codec_exception("drain/send_packet", codec_id(), hw_enabled_);
        av_log_set_level(prev_level);
        return;
    }
    while (avcodec_receive_frame(codec_ctx_, frame) >= 0) {
        if (cancelled_.load(std::memory_order_acquire)) {
            av_frame_unref(frame);
            break;
        }
        if (target_us >= 0) {
            rescale_ts(frame);
            flush_hw_before_conversion_if_needed();
            TextureFrame tex_frame = converter_.convert(frame);
            if (tex_frame.pts_us >= target_us - kExactSeekLookbehindUs) {
                exact_seek_reorder_.push_back(std::move(tex_frame));
            }
        }
        av_frame_unref(frame);
    }
    safe_flush_codec();
    av_log_set_level(prev_level);
    eof_flushed_ = true;
}

void DecodeThread::safe_flush_codec() {
    if (!codec_ctx_) {
        return;
    }
    avcodec_flush_buffers(codec_ctx_);
    if (hw_enabled_ && hw_provider_) {
        hw_provider_->flush();
    }
}

void DecodeThread::flush_hw_visibility_if_needed() {
    if (!hw_enabled_ || !hw_provider_ || !hw_visibility_flush_pending_) {
        return;
    }
    hw_provider_->flush();
    hw_visibility_flush_pending_ = false;
}

void DecodeThread::flush_hw_before_conversion_if_needed() {
    if (!hw_enabled_ || !hw_provider_ || !converter_.downloads_hardware_to_cpu()) {
        return;
    }
    hw_provider_->flush();
    hw_visibility_flush_pending_ = false;
}

void DecodeThread::flush_reorder_buffer() {
    if (exact_seek_reorder_.empty()) return;
    std::sort(exact_seek_reorder_.begin(), exact_seek_reorder_.end(),
              [](const TextureFrame& a, const TextureFrame& b) {
                  return a.pts_us < b.pts_us;
              });
    // Make the decode-device writes visible before exposing reordered frames
    // to the render thread; otherwise the paused preview can sample a
    // partially-written first seek frame.
    flush_hw_visibility_if_needed();
    for (auto& f : exact_seek_reorder_) {
        output_buffer_.push_frame(std::move(f));
    }
    spdlog::info("[DecodeThread] Exact seek reorder: {} frames sorted and pushed",
                 exact_seek_reorder_.size());
    exact_seek_reorder_.clear();
    exact_seek_target_us_ = -1;
}

void DecodeThread::publish_exact_seek_window(size_t selected) {
    if (selected >= exact_seek_reorder_.size()) {
        return;
    }

    flush_hw_visibility_if_needed();
    const int64_t pts = exact_seek_reorder_[selected].pts_us;
    const size_t end = std::min(exact_seek_reorder_.size(),
                                selected + kExactSeekPreviewWindowFrames);
    const size_t published = end - selected;
    for (size_t i = selected; i < end; ++i) {
        output_buffer_.push_frame(std::move(exact_seek_reorder_[i]));
    }
    output_buffer_.set_state(TrackState::Ready);
    if (pause_after_preroll_.load(std::memory_order_acquire)) {
        decode_paused_.store(true, std::memory_order_release);
    }
    post_seek_ = false;
    exact_seek_target_us_ = -1;
    exact_seek_reorder_.clear();
    spdlog::info("[DecodeThread] Exact seek drain: preview frame ready pts={:.3f}s, published={} frames, state->Ready",
                 pts / 1e6, published);
}

bool DecodeThread::publish_best_exact_seek_frame() {
    if (exact_seek_target_us_ < 0 || exact_seek_reorder_.empty()) {
        return false;
    }

    std::sort(exact_seek_reorder_.begin(), exact_seek_reorder_.end(),
              [](const TextureFrame& a, const TextureFrame& b) {
                  return a.pts_us < b.pts_us;
              });

    size_t selected = 0;
    bool found_before_target = false;
    for (size_t i = 0; i < exact_seek_reorder_.size(); ++i) {
        if (exact_seek_reorder_[i].pts_us < exact_seek_target_us_) {
            selected = i;
            found_before_target = true;
        } else {
            break;
        }
    }

    if (!found_before_target) {
        selected = 0;
    }

    const int64_t selected_pts = exact_seek_reorder_[selected].pts_us;
    const size_t collected = exact_seek_reorder_.size();
    spdlog::info("[DecodeThread] Exact seek reorder: selected pts={:.3f}s from {} frames (target={:.3f}s)",
                 selected_pts / 1e6, collected, exact_seek_target_us_ / 1e6);
    publish_exact_seek_window(selected);
    return true;
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
        auto preroll_ready = [&] {
            return post_seek_
                ? output_buffer_.total_count() >= post_seek_preroll_target(hw_enabled_)
                : output_buffer_.has_preroll();
        };

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

            av_frame_unref(frame);
            exact_seek_reorder_.clear();

            const bool fresh_codec_seek =
                perf_.frames_decoded.load(std::memory_order_relaxed) == 0 &&
                output_buffer_.total_count() == 0;
            if (fresh_codec_seek) {
                spdlog::info("[DecodeThread] Seek flush: skipped for fresh codec (hw={})", hw_enabled_);
            } else {
                safe_flush_codec();
                spdlog::info("[DecodeThread] Seek flush: codec buffers flushed (hw={})", hw_enabled_);
            }

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
            hw_visibility_flush_pending_ = hw_enabled_;

            eof_flushed_ = false;
            decode_paused_.store(false, std::memory_order_release);
            output_buffer_.set_state(TrackState::Buffering);
            spdlog::info("[DecodeThread] === SEEK DONE: state->Buffering, post_seek fast preroll, waiting for new packets");
            continue;
        }

        // Fully pause decode consumption so the packet queue preserves packets
        // and the demux thread stops at backpressure instead of racing to EOF.
        if (decode_paused_.load(std::memory_order_acquire) &&
            output_buffer_.state() != TrackState::Flushing) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Non-blocking pop with short sleep — allows seek_pending to be checked promptly
        AVPacket* pkt = input_queue_.try_pop();
        if (!pkt) {
            if (!running_.load(std::memory_order_acquire) ||
                cancelled_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
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
                    if (exact_seek_target_us_ >= 0) {
                        publish_best_exact_seek_frame();
                    } else {
                        flush_reorder_buffer();
                    }
                    // Preroll check — may complete if reorder flush added frames.
                    // Even with 0 frames, transition to Ready: the seek target is past
                    // this track's duration, no frames will ever arrive here.
                    if (post_seek_) {
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
                    try {
                        avcodec_send_packet(codec_ctx_, nullptr);
                    } catch (...) {
                        output_buffer_.set_state(TrackState::Error);
                        decode_paused_.store(true, std::memory_order_release);
                        running_.store(false, std::memory_order_release);
                        log_codec_exception("eof_drain/send_packet", codec_id(), hw_enabled_);
                        break;
                    }
                    while (true) {
                        int ret = 0;
                        try {
                            ret = avcodec_receive_frame(codec_ctx_, frame);
                        } catch (...) {
                            output_buffer_.set_state(TrackState::Error);
                            decode_paused_.store(true, std::memory_order_release);
                            running_.store(false, std::memory_order_release);
                            log_codec_exception("eof_drain/receive_frame", codec_id(), hw_enabled_);
                            ret = AVERROR_EXTERNAL;
                        }
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                        if (ret < 0) break;

                        rescale_ts(frame);
                        flush_hw_before_conversion_if_needed();
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
        try {
            if (hw_enabled_ && device_mutex_) {
                std::lock_guard<std::recursive_mutex> d3d_lock(*device_mutex_);
                ret = avcodec_send_packet(codec_ctx_, pkt);
            } else {
                ret = avcodec_send_packet(codec_ctx_, pkt);
            }
        } catch (...) {
            av_packet_free(&pkt);
            output_buffer_.set_state(TrackState::Error);
            decode_paused_.store(true, std::memory_order_release);
            running_.store(false, std::memory_order_release);
            log_codec_exception("send_packet", codec_id(), hw_enabled_);
            break;
        }
        av_packet_free(&pkt);

        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
            spdlog::error("[DecodeThread] Error sending packet: {:#x}", static_cast<unsigned>(ret));
            continue;
        }

        int frames_produced = 0;
        while (true) {
            if (cancelled_.load(std::memory_order_acquire)) break;
            try {
                if (hw_enabled_ && device_mutex_) {
                    std::lock_guard<std::recursive_mutex> d3d_lock(*device_mutex_);
                    ret = avcodec_receive_frame(codec_ctx_, frame);
                } else {
                    ret = avcodec_receive_frame(codec_ctx_, frame);
                }
            } catch (...) {
                output_buffer_.set_state(TrackState::Error);
                decode_paused_.store(true, std::memory_order_release);
                running_.store(false, std::memory_order_release);
                log_codec_exception("receive_frame", codec_id(), hw_enabled_);
                ret = AVERROR_EXTERNAL;
                break;
            }
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }
            if (ret < 0) {
                spdlog::error("[DecodeThread] Error receiving frame: {:#x}", static_cast<unsigned>(ret));
                break;
            }

            rescale_ts(frame);

            // Exact seek: collect a small PTS window around the target, then
            // publish the closest frame before target. HEVC can output B-frames
            // after a later reference frame, so publishing on the first
            // frame >= target can incorrectly jump back to the keyframe.
            if (exact_seek_target_us_ >= 0) {
                if (frame->pts < exact_seek_target_us_ - kExactSeekLookbehindUs) {
                    perf_.frames_dropped.fetch_add(1, std::memory_order_relaxed);
                    av_frame_unref(frame);
                    continue;
                }

                flush_hw_before_conversion_if_needed();
                TextureFrame tex_frame = converter_.convert(frame);
                ++frames_produced;
                exact_seek_reorder_.push_back(std::move(tex_frame));

                bool has_frame_at_or_after_target = false;
                int64_t max_pts = -1;
                for (const auto& f : exact_seek_reorder_) {
                    has_frame_at_or_after_target =
                        has_frame_at_or_after_target ||
                        f.pts_us >= exact_seek_target_us_;
                    max_pts = std::max(max_pts, f.pts_us);
                }

                if (has_frame_at_or_after_target &&
                    (exact_seek_reorder_.size() >= kExactSeekReorderPublishFrames ||
                     max_pts >= exact_seek_target_us_ + kExactSeekLeadUs)) {
                    publish_best_exact_seek_frame();
                    av_frame_unref(frame);
                    break;
                }

                av_frame_unref(frame);
                continue;
            }

            flush_hw_before_conversion_if_needed();
            TextureFrame tex_frame = converter_.convert(frame);
            ++frames_produced;

            // Flush the independent decode device after the first visible HW
            // frame on startup and after seek/add-track transitions. Without
            // this, the render device can sample a partially-written NV12
            // surface, which shows up as green or missing regions.
            if (frames_produced == 1 &&
                perf_.frames_decoded.load(std::memory_order_relaxed) == 0) {
                hw_visibility_flush_pending_ = hw_enabled_;
            }

            // The flush must happen before push_frame() publishes this frame
            // to the render thread, otherwise the paused preview path can win
            // the race and draw an incomplete surface.
            flush_hw_visibility_if_needed();
            output_buffer_.push_frame(std::move(tex_frame));

            if (output_buffer_.state() == TrackState::Buffering) {
                if (preroll_ready()) {
                    spdlog::info("[DecodeThread] === Preroll complete: {} frames buffered, post_seek={}, state->Ready",
                                 output_buffer_.total_count(), post_seek_);
                    output_buffer_.set_state(TrackState::Ready);
                    if (pause_after_preroll_.load(std::memory_order_acquire)) {
                        decode_paused_.store(true, std::memory_order_release);
                    }
                    post_seek_ = false;
                }
            }

            av_frame_unref(frame);
        }

        // Exact seek B-frame reordering fallback. The receive loop normally
        // publishes once enough frames are collected, but EOF/drain can also
        // make the buffer ready here.
        if (exact_seek_target_us_ >= 0 && !exact_seek_reorder_.empty()) {
            bool should_publish = false;

            if (input_queue_.is_eof() && input_queue_.size() == 0) {
                if (!eof_flushed_) {
                    drain_codec(frame, rescale_ts, exact_seek_target_us_);
                    spdlog::info("[DecodeThread] Exact seek EOF: codec drain, reorder buffer now has {} frames",
                                 exact_seek_reorder_.size());
                }
                should_publish = true;
            } else {
                bool has_frame_at_or_after_target = false;
                int64_t max_pts = -1;
                for (const auto& f : exact_seek_reorder_) {
                    has_frame_at_or_after_target =
                        has_frame_at_or_after_target ||
                        f.pts_us >= exact_seek_target_us_;
                    max_pts = std::max(max_pts, f.pts_us);
                }
                if (has_frame_at_or_after_target &&
                    (exact_seek_reorder_.size() >= kExactSeekReorderPublishFrames ||
                     max_pts >= exact_seek_target_us_ + kExactSeekLeadUs)) {
                    should_publish = true;
                }
            }

            if (should_publish) {
                publish_best_exact_seek_frame();
                auto first = output_buffer_.peek(0);
                spdlog::info("[DecodeThread] Exact seek reorder: frames pushed, first_pts={:.3f}s",
                             first.has_value() ? first->pts_us / 1e6 : -1.0);
            }
        }

        // Preroll check
        if (output_buffer_.state() == TrackState::Buffering) {
            if (preroll_ready()) {
                spdlog::info("[DecodeThread] === Preroll complete: {} frames buffered, post_seek={}, state->Ready",
                             output_buffer_.total_count(), post_seek_);
                output_buffer_.set_state(TrackState::Ready);
                if (pause_after_preroll_.load(std::memory_order_acquire)) {
                    decode_paused_.store(true, std::memory_order_release);
                }
                post_seek_ = false;
            }
        }

        // D3D11VA HEVC exact seek is sensitive to burst-feeding packets while
        // paused. Playback naturally paces this path through render/clock
        // consumption; mirror a tiny amount of that pacing during drain mode.
        if (exact_seek_target_us_ >= 0 && hw_enabled_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
