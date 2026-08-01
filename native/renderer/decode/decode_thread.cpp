#include "renderer/decode/decode_thread.h"
#include "renderer/decode/av_frame_lifetime.h"
#include "renderer/decode/codec_loop.h"
#include "renderer/decode/decode_preroll_policy.h"
#include "renderer/decode/exact_seek_frame_publisher.h"
#include "renderer/decode/exact_seek_publish_policy.h"
#include "renderer/decode/exact_seek_window.h"
#include <spdlog/spdlog.h>
#include <sstream>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace vr {

namespace {
uint64_t estimate_av_yuv_surface_bytes(int width, int height, AVPixelFormat format) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const uint64_t pixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    switch (format) {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        return pixels * 3 / 2;
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P016LE:
    case AV_PIX_FMT_YUV420P10LE:
        return pixels * 3;
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUVJ422P:
        return pixels * 2;
    case AV_PIX_FMT_YUV422P10LE:
        return pixels * 4;
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P:
        return pixels * 3;
    case AV_PIX_FMT_YUV444P10LE:
        return pixels * 6;
    default:
        return 0;
    }
}

bool renderer_owned_metal_supports_stream_format(AVPixelFormat format) {
    switch (format) {
    case AV_PIX_FMT_NONE:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21:
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_P010LE:
        return true;
    default:
        return false;
    }
}

}  // namespace

DecodeThread::DecodeThread(PacketQueue& input_queue, TrackBuffer& output_buffer,
                           const AVCodecParameters* codec_params, AVRational time_base)
    : input_queue_(input_queue)
    , output_buffer_(output_buffer)
    , time_base_(time_base)
    , timestamp_normalizer_(time_base)
{
    VideoDecodeSessionOptions options;
    std::string error;
    if (!decoder_.initialize(codec_params, options, error)) {
        spdlog::error("[DecodeThread] {}", error);
        return;
    }

    // NOTE: avcodec_open2 is NOT called here.
    // It is deferred to start() so that enable_hardware_decode() can set
    // hw_device_ctx before the codec is opened.
}

DecodeThread::~DecodeThread() {
    stop();
}

bool DecodeThread::enable_hardware_decode(DecodeDeviceMode mode,
                                           void* render_device,
                                           std::recursive_mutex* device_mutex,
                                           RenderBackendKind backend) {
    if (!decoder_.is_valid()) {
        spdlog::warn("[DecodeThread] Cannot enable hw decode: codec not initialized");
        return false;
    }

    if (mode == DecodeDeviceMode::SharedRenderDevice && !render_device) {
        spdlog::warn("[DecodeThread] SharedRenderDevice requested without render_device");
        return false;
    }

    const auto* codec_params = decoder_.codec_parameters();
    const auto stream_format =
        static_cast<AVPixelFormat>(codec_params->format);
    if (backend == RenderBackendKind::Metal &&
        mode != DecodeDeviceMode::FfmpegOwnedHwDownloadDevice &&
        !renderer_owned_metal_supports_stream_format(stream_format)) {
        const char* name = av_get_pix_fmt_name(stream_format);
        spdlog::info("[DecodeThread] Hardware decode disabled for stream pixel format {} ({}) "
                     "because renderer-owned Metal path only supports NV12/P010-like 4:2:0 surfaces",
                     static_cast<int>(stream_format), name ? name : "unknown");
        return false;
    }
    std::string diagnostic;
    const bool enabled = decoder_.enable_hardware_decode(
        mode,
        render_device,
        device_mutex,
        backend,
        &diagnostic);
    if (!enabled) {
        spdlog::info(
            "[DecodeThread] Hardware decode unavailable: {}",
            diagnostic);
    }
    return enabled;
}

bool DecodeThread::open_codec() {
    std::string error;
    if (decoder_.open(error)) {
        return true;
    }
    spdlog::error("[DecodeThread] Failed to open codec: {}",
                  error);
    return false;
}

bool DecodeThread::hardware_output_downloads_to_cpu() const {
    return decoder_.hardware_output_downloads_to_cpu();
}

bool DecodeThread::start() {
    if (!decoder_.is_valid()) {
        spdlog::error("[DecodeThread] Cannot start: codec not initialized");
        return false;
    }
    if (running_.load()) return false;

    // Open codec (deferred from constructor so hw_device_ctx can be set first)
    if (!open_codec()) {
        spdlog::error("[DecodeThread] Cannot start: codec open failed");
        return false;
    }

    AVCodecContext* codec_context = decoder_.codec_context();
    spdlog::info("[DecodeThread] Codec opened successfully ({}x{})",
                 codec_context->width, codec_context->height);

    // Initialize the frame converter based on decode mode
    bool conv_ok;
    if (decoder_.hardware_enabled()) {
        conv_ok = converter_.init_hardware(decoder_.native_device(), nullptr,
                                           codec_context->width, codec_context->height,
                                           decoder_.hardware_type(),
                                           hardware_output_downloads_to_cpu(),
                                           decoder_.device_mutex());
    } else {
        conv_ok = converter_.init_software(codec_context->width,
                                           codec_context->height,
                                           codec_context->pix_fmt);
    }
    if (!conv_ok) {
        spdlog::error("[DecodeThread] Failed to initialize frame converter");
        return false;
    }

    output_buffer_.set_state(TrackState::Buffering);
    timestamp_normalizer_.reset();
    hw_visibility_flush_pending_ = decoder_.hardware_enabled();
    running_.store(true);
    thread_ = std::thread(&DecodeThread::run, this);
    return true;
}

std::string DecodeThread::decoder_name() const {
    const AVCodec* codec = decoder_.codec();
    const char* codec_name = codec && codec->name ? codec->name : "";
    if (!decoder_.hardware_enabled()) {
        return codec_name;
    }
    const auto* provider = decoder_.hardware_provider();
    const char* hw_name = provider ? provider->name() : "hardware";
    std::ostringstream oss;
    oss << hw_name << " / " << codec_name;
    return oss.str();
}

DecodeMemoryStats DecodeThread::memory_stats() const {
    DecodeMemoryStats stats;
    stats.hardware_enabled = decoder_.hardware_enabled();
    stats.hardware_download_to_cpu = converter_.downloads_hardware_to_cpu();
    stats.hw_format = hw_frames_format_.load(std::memory_order_relaxed);
    stats.sw_format = hw_frames_sw_format_.load(std::memory_order_relaxed);
    stats.hw_width = hw_frames_width_.load(std::memory_order_relaxed);
    stats.hw_height = hw_frames_height_.load(std::memory_order_relaxed);
    stats.hw_initial_pool_size =
        hw_frames_initial_pool_size_.load(std::memory_order_relaxed);
    const AVCodecContext* codec_context = decoder_.codec_context();
    stats.extra_hw_frames =
        codec_context ? codec_context->extra_hw_frames : 0;
    stats.estimated_hw_frame_bytes = estimate_av_yuv_surface_bytes(
        stats.hw_width,
        stats.hw_height,
        static_cast<AVPixelFormat>(stats.sw_format));
    if (stats.hw_initial_pool_size > 0) {
        stats.estimated_hw_pool_bytes =
            stats.estimated_hw_frame_bytes *
            static_cast<uint64_t>(stats.hw_initial_pool_size);
    }
    stats.snapshot_pool = converter_.snapshot_pool_stats();
    const auto exact_stats = exact_seek_candidates_.stats_snapshot();
    stats.exact_seek_reorder_count = exact_stats.reorder_count;
    stats.exact_seek_pending_count = exact_stats.pending_count;
    stats.exact_seek_stable_frame_count = exact_stats.stable_frame_count;
    stats.exact_seek_budget_drop_count = exact_stats.dropped_by_budget_count;
    stats.exact_seek_candidate_cpu_bytes = exact_stats.candidate_cpu_bytes;
    stats.exact_seek_stable_cpu_bytes = exact_stats.stable_cpu_bytes;
    return stats;
}

void DecodeThread::stop() {
    spdlog::debug("[DecodeThread] stop() begin");
    running_.store(false);
    cancelled_.store(true, std::memory_order_release);
    input_queue_.clear_eof();
    input_queue_.abort();   // Unblock blocking pop
    output_buffer_.abort(); // Unblock blocking push_frame
    if (thread_.joinable()) {
        spdlog::debug("[DecodeThread] stop() waiting for decode thread join");
        thread_.join();
        spdlog::debug("[DecodeThread] stop() decode thread joined");
    }
    exact_seek_candidates_.clear();
    // Release output frames BEFORE freeing hw resources.
    // TextureFrames hold hw_frame_ref (av_frame_ref) which reference
    // hw_frames_ctx -> hw_device_ctx. If hw_device_ctx is freed first,
    // the frame cleanup will access a freed device context (SIGSEGV).
    spdlog::debug("[DecodeThread] stop() clearing output frames");
    output_buffer_.clear_frames();
    spdlog::debug("[DecodeThread] stop() output frames cleared");

    spdlog::debug(
        "[DecodeThread] stop() releasing shared decode session");
    decoder_.close();
    spdlog::debug("[DecodeThread] stop() end");
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

std::optional<DecodeSeekNotification> DecodeThread::take_pending_seek_notification() {
    std::lock_guard<std::mutex> lock(seek_mutex_);
    return take_pending_decode_seek(seek_);
}

bool DecodeThread::has_pending_seek_notification() {
    std::lock_guard<std::mutex> lock(seek_mutex_);
    return seek_.pending;
}

void DecodeThread::begin_seek_epoch(AVFrame* frame, const DecodeSeekNotification& notification) {
    cancelled_.store(false, std::memory_order_release);
    timestamp_normalizer_.reset();

    spdlog::debug("[DecodeThread] === SEEK START: target={:.3f}s, type={}, "
                 "input_pq={}, output_buf={}, buf_state={}",
                 notification.target_pts_us / 1e6,
                 decode_seek_type_name(notification.type),
                 input_queue_.size(),
                 output_buffer_.total_count(),
                 static_cast<int>(output_buffer_.state()));

    reset_reusable_av_frame(frame);
    exact_seek_candidates_.clear();
    drain_decoder_before_next_packet_ = false;

    // Always reset codec state on seek. During add-track initial seek,
    // demux can race ahead and the decoder may have already accepted
    // packets even though no frames have been published yet.
    safe_flush_codec();
    spdlog::debug("[DecodeThread] Seek flush: codec buffers flushed (hw={})",
                 decoder_.hardware_enabled());

    // NOTE: Do NOT drain input queue here! The DemuxThread already
    // flushes the queue before seeking and then pushes NEW packets.
    // Draining here would discard those fresh post-seek packets.
    const auto state = build_decode_seek_epoch_start_state(
        notification, decoder_.hardware_enabled());
    exact_seek_target_us_ = state.exact_seek_target_us;
    exact_seek_prefer_after_target_ = is_step_forward_seek_type(notification.type);
    if (is_exact_seek_type(notification.type)) {
        spdlog::debug("[DecodeThread] Exact seek: will discard frames < {:.3f}s",
                     notification.target_pts_us / 1e6);
    }

    post_seek_ = state.post_seek;
    hw_visibility_flush_pending_ = state.hw_visibility_flush_pending;
    eof_flushed_ = state.eof_flushed;
    decode_paused_.store(state.decode_paused, std::memory_order_release);
    output_buffer_.set_state(state.output_state);
    spdlog::debug("[DecodeThread] === SEEK DONE: state->Buffering, post_seek fast preroll, waiting for new packets");
}

void DecodeThread::drain_codec(AVFrame* frame, const std::function<void(AVFrame*)>& rescale_ts, int64_t target_us) {
    int send_ret = decoder_.send_packet(nullptr);
    if (send_ret < 0 && send_ret != AVERROR(EAGAIN) && send_ret != AVERROR_EOF) {
        if (codec_loop_is_seh_caught(send_ret)) {
            output_buffer_.set_state(TrackState::Error);
            running_.store(false, std::memory_order_release);
        }
        return;
    }
    while (true) {
        int recv_ret = decoder_.receive_frame(frame);
        if (recv_ret < 0) break;
        AvFrameUnrefGuard frame_guard(frame);
        if (cancelled_.load(std::memory_order_acquire)) {
            break;
        }
        if (target_us >= 0) {
            rescale_ts(frame);
            if (should_collect_exact_seek_candidate(frame->pts, target_us)) {
                auto candidate = ExactSeekCandidateStore::make_candidate(frame);
                if (candidate.frame) {
                    collect_exact_seek_candidate(std::move(candidate));
                }
            }
        }
    }
    safe_flush_codec();
    eof_flushed_ = true;
}

void DecodeThread::safe_flush_codec() {
    if (!decoder_.is_valid()) {
        return;
    }
    decoder_.flush();
}

void DecodeThread::flush_reorder_buffer() {
    exact_seek_candidates_.clear_pending();
    drain_decoder_before_next_packet_ = false;
    if (exact_seek_candidates_.reorder_empty()) {
        return;
    }
    // Make the decode-device writes visible before exposing decoder-ordered
    // candidate frames
    // to the render thread; otherwise the paused preview can sample a
    // partially-written first seek frame.
    auto publisher = make_frame_publisher();
    publisher.flush_visibility_if_needed();
    const size_t pushed_count = exact_seek_candidates_.reorder_count();
    for (auto& f : exact_seek_candidates_.reorder_candidates()) {
        if (!f.frame) {
            continue;
        }
        publisher.flush_before_publish_if_needed(true);
        if (!publisher.convert_and_push_frame(f.frame.get(), "exact-seek reorder flush")) {
            break;
        }
    }
    spdlog::debug("[DecodeThread] Exact seek candidate window: {} frames pushed",
                 pushed_count);
    exact_seek_candidates_.clear_reorder();
    exact_seek_target_us_ = -1;
}

void DecodeThread::snapshot_exact_seek_candidate_if_needed(ExactSeekCandidate& candidate) {
    if (!candidate.frame || !decoder_.hardware_enabled() ||
        converter_.downloads_hardware_to_cpu()) {
        return;
    }
    auto stable = converter_.snapshot_hardware_frame(candidate.frame.get());
    if (stable.has_value() && stable->texture_handle) {
        candidate.stable_frame = std::move(stable);
    }
}

void DecodeThread::collect_exact_seek_candidate(ExactSeekCandidate candidate) {
    exact_seek_candidates_.collect(
        std::move(candidate),
        exact_seek_target_us_,
        [this](ExactSeekCandidate& candidate_to_snapshot) {
            snapshot_exact_seek_candidate_if_needed(candidate_to_snapshot);
        });
}

void DecodeThread::log_hw_frame_context_once(const AVFrame* frame) {
    if (!decoder_.hardware_enabled() || hw_frames_ctx_logged_ ||
        !frame || !frame->hw_frames_ctx) {
        return;
    }
    auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    if (!frames_ctx) {
        return;
    }
    hw_frames_format_.store(static_cast<int>(frames_ctx->format), std::memory_order_relaxed);
    hw_frames_sw_format_.store(static_cast<int>(frames_ctx->sw_format), std::memory_order_relaxed);
    hw_frames_width_.store(frames_ctx->width, std::memory_order_relaxed);
    hw_frames_height_.store(frames_ctx->height, std::memory_order_relaxed);
    hw_frames_initial_pool_size_.store(frames_ctx->initial_pool_size, std::memory_order_relaxed);
    spdlog::info("[DecodeThread] HW frames ctx: format={}, sw_format={}, {}x{}, initial_pool_size={}, extra_hw_frames={}",
                 static_cast<int>(frames_ctx->format),
                 static_cast<int>(frames_ctx->sw_format),
                 frames_ctx->width,
                 frames_ctx->height,
                 frames_ctx->initial_pool_size,
                 decoder_.codec_context()
                     ? decoder_.codec_context()->extra_hw_frames
                     : 0);
    hw_frames_ctx_logged_ = true;
}

bool DecodeThread::exact_seek_preview_window_ready() const {
    return exact_seek_candidates_.preview_window_ready(exact_seek_target_us_);
}

void DecodeThread::publish_exact_seek_window(size_t selected) {
    if (selected >= exact_seek_candidates_.reorder_count()) {
        return;
    }

    auto publisher = make_frame_publisher();
    const auto publish_result = publish_exact_seek_preview_frames(
        exact_seek_candidates_,
        selected,
        output_buffer_,
        publisher);
    const auto completion = plan_exact_seek_preview_completion(
        publish_result.can_publish,
        publish_result.conversion_failed,
        pause_after_preroll_.load(std::memory_order_acquire),
        publish_result.selected_pts_us,
        publish_result.published_count,
        publish_result.pending_count);
    if (!completion.apply) {
        return;
    }
    output_buffer_.set_state(completion.output_state);
    if (completion.pause_decode) {
        decode_paused_.store(true, std::memory_order_release);
    }
    post_seek_ = completion.post_seek;
    exact_seek_target_us_ = completion.exact_seek_target_us;
    drain_decoder_before_next_packet_ = completion.drain_decoder_before_next_packet;
    spdlog::debug("[DecodeThread] Exact seek drain: preview frame ready pts={:.3f}s, published={} frames, pending={} frames, state->Ready",
                 completion.selected_pts_us / 1e6,
                 completion.published_count,
                 completion.pending_count);
}

bool DecodeThread::publish_best_exact_seek_frame() {
    if (exact_seek_target_us_ < 0 || exact_seek_candidates_.reorder_empty()) {
        return false;
    }

    auto candidate_pts_us = exact_seek_candidates_.reorder_pts();
    const auto selected = select_exact_seek_preview_index(
        candidate_pts_us, exact_seek_target_us_, exact_seek_prefer_after_target_);
    if (!selected.has_value()) {
        return false;
    }

    const int64_t selected_pts = exact_seek_candidates_.reorder_at(*selected).pts_us;
    const size_t collected = exact_seek_candidates_.reorder_count();
    spdlog::debug("[DecodeThread] Exact seek candidate window: selected pts={:.3f}s from {} decoder-ordered frames (target={:.3f}s)",
                 selected_pts / 1e6, collected, exact_seek_target_us_ / 1e6);
    publish_exact_seek_window(*selected);
    return true;
}

void DecodeThread::publish_pending_exact_seek_frames() {
    auto publisher = make_frame_publisher();
    publish_pending_exact_seek_frame(exact_seek_candidates_, publisher);
}

bool DecodeThread::complete_preroll_if_ready() {
    const auto output_state = output_buffer_.state();
    if (output_state != TrackState::Buffering) {
        return false;
    }

    const bool preroll_ready = is_decode_preroll_ready(
        post_seek_,
        decoder_.hardware_enabled(),
        output_buffer_.total_count(),
        output_buffer_.has_preroll());
    const auto decision = choose_decode_preroll_transition(
        output_state,
        preroll_ready,
        pause_after_preroll_.load(std::memory_order_acquire));
    if (!decision.complete) {
        return false;
    }

    spdlog::debug("[DecodeThread] === Preroll complete: {} frames buffered, post_seek={}, state->Ready",
                 output_buffer_.total_count(), post_seek_);
    output_buffer_.set_state(decision.output_state);
    if (decision.pause_decode) {
        decode_paused_.store(true, std::memory_order_release);
    }
    if (decision.clear_post_seek) {
        post_seek_ = false;
    }
    return true;
}

} // namespace vr
