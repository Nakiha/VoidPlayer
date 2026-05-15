#include "video_renderer/decode/decode_thread.h"
#include "video_renderer/decode/codec_loop.h"
#include "video_renderer/decode/decode_loop_policy.h"
#include "video_renderer/decode/exact_seek_window.h"
#include <spdlog/spdlog.h>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <windows.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

namespace vr {

namespace {
constexpr int kRendererOwnedHwExtraFrames = 0;

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

uint64_t estimate_av_frame_cpu_bytes(const AVFrame* frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0 ||
        frame->format == AV_PIX_FMT_NONE || frame->hw_frames_ctx) {
        return 0;
    }
    const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    if (!desc) {
        return 0;
    }

    int max_plane = 0;
    for (int i = 0; i < desc->nb_components; ++i) {
        max_plane = std::max(max_plane, static_cast<int>(desc->comp[i].plane));
    }

    uint64_t bytes = 0;
    for (int plane = 0; plane <= max_plane && plane < AV_NUM_DATA_POINTERS; ++plane) {
        if (!frame->data[plane] || frame->linesize[plane] == 0) {
            continue;
        }
        const bool chroma_plane = plane == 1 || plane == 2;
        const int shift = chroma_plane ? desc->log2_chroma_h : 0;
        const int plane_height = (frame->height + (1 << shift) - 1) >> shift;
        const int stride = frame->linesize[plane] < 0
            ? -frame->linesize[plane]
            : frame->linesize[plane];
        bytes += static_cast<uint64_t>(stride) * static_cast<uint64_t>(plane_height);
    }
    return bytes;
}

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

const char* decode_device_mode_name(DecodeDeviceMode mode) {
    switch (mode) {
    case DecodeDeviceMode::IndependentDevice:
        return "IndependentDevice";
    case DecodeDeviceMode::SharedRenderDevice:
        return "SharedRenderDevice";
    case DecodeDeviceMode::FfmpegOwnedHwDownloadDevice:
        return "FfmpegOwnedHwDownloadDevice";
    }
    return "Unknown";
}

bool renderer_owned_d3d11_supports_stream_format(AVPixelFormat format) {
    switch (format) {
    case AV_PIX_FMT_NONE:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_NV21:
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P016LE:
    case AV_PIX_FMT_YUV420P12LE:
    case AV_PIX_FMT_P012LE:
        return true;
    default:
        return false;
    }
}

__declspec(noinline)
int seh_open_codec(AVCodecContext* ctx,
                   const AVCodec* codec,
                   AVDictionary** options,
                   DecodeThread::CodecOpenFunction open_fn) {
    __try {
        if (open_fn) {
            return open_fn(ctx, codec, options);
        }
        return avcodec_open2(ctx, codec, options);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        spdlog::error("[DecodeThread] SEH exception in avcodec_open2: {:#x}",
                      static_cast<unsigned long>(code));
        return codec_loop_seh_caught_code();
    }
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
    if (!codec_params) {
        spdlog::error("[DecodeThread] codec_params is null");
        return;
    }
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

bool DecodeThread::enable_hardware_decode(DecodeDeviceMode mode,
                                           void* render_device,
                                           std::recursive_mutex* device_mutex) {
    if (!codec_ctx_ || !codec_) {
        spdlog::warn("[DecodeThread] Cannot enable hw decode: codec not initialized");
        return false;
    }

    if (mode == DecodeDeviceMode::SharedRenderDevice && !render_device) {
        spdlog::warn("[DecodeThread] SharedRenderDevice requested without render_device");
        return false;
    }

    decode_device_mode_ = mode;
    native_device_ = (mode == DecodeDeviceMode::SharedRenderDevice) ? render_device : nullptr;
    device_mutex_ = device_mutex;

    const auto stream_format = static_cast<AVPixelFormat>(codec_params_->format);
    if (mode != DecodeDeviceMode::FfmpegOwnedHwDownloadDevice &&
        !renderer_owned_d3d11_supports_stream_format(stream_format)) {
        const char* name = av_get_pix_fmt_name(stream_format);
        spdlog::info("[DecodeThread] Hardware decode disabled for stream pixel format {} ({}) "
                     "because renderer-owned D3D11 path only supports NV12/P010-like 4:2:0 surfaces",
                     static_cast<int>(stream_format), name ? name : "unknown");
        hw_enabled_ = false;
        const AVCodec* sw_codec = preferred_software_decoder();
        if (sw_codec && sw_codec != codec_) {
            spdlog::info("[DecodeThread] Switching decoder to {} for software fallback",
                         sw_codec->name);
            reset_codec_context(sw_codec);
        }
        return false;
    }

    HwDecodeInitParams hw_params;
    hw_params.backend = RenderBackendType::D3D11;
    hw_params.device_mode = mode;
    hw_params.render_device = render_device;
    hw_params.width = codec_params_->width;
    hw_params.height = codec_params_->height;
    hw_params.device_mutex = device_mutex;

    spdlog::info("[DecodeThread] Hardware decode device mode: {}",
                 decode_device_mode_name(mode));

    auto result = try_hw_decode_providers(codec_, hw_params);

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
        codec_ctx_->extra_hw_frames = kRendererOwnedHwExtraFrames;
        spdlog::info("[DecodeThread] Renderer-owned hardware frame pool extra_hw_frames={}",
                     codec_ctx_->extra_hw_frames);
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

    int ret = seh_open_codec(codec_ctx_, codec_, nullptr, codec_open_for_test_);
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

        int ret2 = seh_open_codec(codec_ctx_, codec_, nullptr, codec_open_for_test_);
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
    return hw_enabled_ &&
           decode_device_mode_ == DecodeDeviceMode::FfmpegOwnedHwDownloadDevice;
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
                                           hardware_output_downloads_to_cpu(),
                                           device_mutex_);
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

std::string DecodeThread::decoder_name() const {
    const char* codec_name = codec_ && codec_->name ? codec_->name : "";
    if (!hw_enabled_) {
        return codec_name;
    }
    const char* hw_name = hw_provider_ ? hw_provider_->name() : "hardware";
    std::ostringstream oss;
    oss << hw_name << " / " << codec_name;
    return oss.str();
}

DecodeMemoryStats DecodeThread::memory_stats() const {
    DecodeMemoryStats stats;
    stats.hardware_enabled = hw_enabled_;
    stats.hardware_download_to_cpu = converter_.downloads_hardware_to_cpu();
    stats.hw_format = hw_frames_format_.load(std::memory_order_relaxed);
    stats.sw_format = hw_frames_sw_format_.load(std::memory_order_relaxed);
    stats.hw_width = hw_frames_width_.load(std::memory_order_relaxed);
    stats.hw_height = hw_frames_height_.load(std::memory_order_relaxed);
    stats.hw_initial_pool_size =
        hw_frames_initial_pool_size_.load(std::memory_order_relaxed);
    stats.extra_hw_frames = codec_ctx_ ? codec_ctx_->extra_hw_frames : 0;
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
    stats.exact_seek_reorder_count =
        exact_seek_reorder_count_.load(std::memory_order_relaxed);
    stats.exact_seek_pending_count =
        exact_seek_pending_count_.load(std::memory_order_relaxed);
    stats.exact_seek_stable_frame_count =
        exact_seek_stable_frame_count_.load(std::memory_order_relaxed);
    stats.exact_seek_candidate_cpu_bytes =
        exact_seek_candidate_cpu_bytes_.load(std::memory_order_relaxed);
    stats.exact_seek_stable_cpu_bytes =
        exact_seek_stable_cpu_bytes_.load(std::memory_order_relaxed);
    return stats;
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
    exact_seek_reorder_.clear();
    exact_seek_pending_frames_.clear();
    refresh_exact_seek_memory_stats();
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
    int send_ret = send_codec_packet_seh_guarded(codec_ctx_, nullptr);
    if (send_ret < 0 && send_ret != AVERROR(EAGAIN) && send_ret != AVERROR_EOF) {
        if (codec_loop_is_seh_caught(send_ret)) {
            output_buffer_.set_state(TrackState::Error);
            running_.store(false, std::memory_order_release);
        }
        return;
    }
    while (true) {
        int recv_ret = receive_codec_frame_seh_guarded(codec_ctx_, frame);
        if (recv_ret < 0) break;
        if (cancelled_.load(std::memory_order_acquire)) {
            av_frame_unref(frame);
            break;
        }
        if (target_us >= 0) {
            rescale_ts(frame);
            if (should_collect_exact_seek_candidate(frame->pts, target_us)) {
                auto candidate = make_exact_seek_candidate(frame);
                if (candidate.frame) {
                    collect_exact_seek_candidate(std::move(candidate));
                }
            }
        }
        av_frame_unref(frame);
    }
    safe_flush_codec();
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

void DecodeThread::flush_hw_before_publish_if_needed(bool force_for_shared_surface) {
    if (!hw_enabled_ || !hw_provider_) {
        return;
    }
    if (!force_for_shared_surface && !converter_.downloads_hardware_to_cpu()) {
        return;
    }
    hw_provider_->flush();
    hw_visibility_flush_pending_ = false;
}

void DecodeThread::flush_reorder_buffer() {
    exact_seek_pending_frames_.clear();
    drain_decoder_before_next_packet_ = false;
    if (exact_seek_reorder_.empty()) {
        refresh_exact_seek_memory_stats();
        return;
    }
    // Make the decode-device writes visible before exposing reordered frames
    // to the render thread; otherwise the paused preview can sample a
    // partially-written first seek frame.
    flush_hw_visibility_if_needed();
    for (auto& f : exact_seek_reorder_) {
        if (!f.frame) {
            continue;
        }
        flush_hw_before_publish_if_needed(true);
        if (!convert_and_push_frame(f.frame.get(), "exact-seek reorder flush")) {
            break;
        }
    }
    spdlog::info("[DecodeThread] Exact seek reorder: {} frames pushed",
                 exact_seek_reorder_.size());
    exact_seek_reorder_.clear();
    exact_seek_target_us_ = -1;
    refresh_exact_seek_memory_stats();
}

DecodeThread::ExactSeekCandidate DecodeThread::make_exact_seek_candidate(AVFrame* frame) const {
    AVFrame* cloned = av_frame_clone(frame);
    if (!cloned) {
        spdlog::error("[DecodeThread] Failed to clone exact-seek candidate frame");
        return {};
    }
    return ExactSeekCandidate{
        frame->pts,
        std::shared_ptr<AVFrame>(cloned, [](AVFrame* f) {
            av_frame_free(&f);
        }),
    };
}

void DecodeThread::snapshot_exact_seek_candidate_if_needed(ExactSeekCandidate& candidate) {
    if (!candidate.frame || !hw_enabled_ || converter_.downloads_hardware_to_cpu()) {
        return;
    }
    auto stable = converter_.snapshot_hardware_frame(candidate.frame.get());
    if (stable.has_value() && stable->texture_handle) {
        candidate.stable_frame = std::move(stable);
    }
}

void DecodeThread::refresh_exact_seek_memory_stats() {
    size_t stable_count = 0;
    uint64_t candidate_cpu_bytes = 0;
    uint64_t stable_cpu_bytes = 0;
    auto visit = [&](const ExactSeekCandidate& candidate) {
        candidate_cpu_bytes += estimate_av_frame_cpu_bytes(candidate.frame.get());
        if (candidate.stable_frame.has_value()) {
            ++stable_count;
            stable_cpu_bytes += estimate_texture_frame_cpu_bytes(*candidate.stable_frame);
        }
    };
    for (const auto& candidate : exact_seek_reorder_) {
        visit(candidate);
    }
    for (const auto& candidate : exact_seek_pending_frames_) {
        visit(candidate);
    }
    exact_seek_reorder_count_.store(exact_seek_reorder_.size(), std::memory_order_relaxed);
    exact_seek_pending_count_.store(exact_seek_pending_frames_.size(), std::memory_order_relaxed);
    exact_seek_stable_frame_count_.store(stable_count, std::memory_order_relaxed);
    exact_seek_candidate_cpu_bytes_.store(candidate_cpu_bytes, std::memory_order_relaxed);
    exact_seek_stable_cpu_bytes_.store(stable_cpu_bytes, std::memory_order_relaxed);
}

void DecodeThread::collect_exact_seek_candidate(ExactSeekCandidate candidate) {
    if (!candidate.frame) {
        return;
    }
    // FFmpeg receive_frame() has already applied codec reorder for display
    // order. While still before the target, only the latest pre-target frame
    // can be selected, so drop older pre-target candidates immediately.
    if (exact_seek_target_us_ >= 0 && candidate.pts_us < exact_seek_target_us_) {
        exact_seek_reorder_.clear();
    } else if (exact_seek_target_us_ >= 0 && exact_seek_reorder_.empty()) {
        // If the target lands before the first displayable frame, that first
        // post-target candidate becomes the preview.
        snapshot_exact_seek_candidate_if_needed(candidate);
    } else if (exact_seek_target_us_ >= 0 &&
               !exact_seek_reorder_.empty() &&
               exact_seek_reorder_.back().pts_us < exact_seek_target_us_ &&
               !exact_seek_reorder_.back().stable_frame.has_value()) {
        snapshot_exact_seek_candidate_if_needed(exact_seek_reorder_.back());
    }
    exact_seek_reorder_.push_back(std::move(candidate));
    refresh_exact_seek_memory_stats();
}

void DecodeThread::log_hw_frame_context_once(const AVFrame* frame) {
    if (!hw_enabled_ || hw_frames_ctx_logged_ || !frame || !frame->hw_frames_ctx) {
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
                 codec_ctx_ ? codec_ctx_->extra_hw_frames : 0);
    hw_frames_ctx_logged_ = true;
}

bool DecodeThread::exact_seek_preview_window_ready() const {
    const int64_t newest_pts_us = exact_seek_reorder_.empty()
        ? 0
        : exact_seek_reorder_.back().pts_us;
    return is_exact_seek_preview_window_ready(
        exact_seek_target_us_, exact_seek_reorder_.size(), newest_pts_us);
}

void DecodeThread::publish_exact_seek_window(size_t selected) {
    if (selected >= exact_seek_reorder_.size()) {
        return;
    }

    flush_hw_visibility_if_needed();
    const int64_t pts = exact_seek_reorder_[selected].pts_us;
    const size_t buffered = output_buffer_.total_count();
    const size_t capacity = output_buffer_.max_count();
    const size_t free_slots = capacity > buffered ? capacity - buffered : 0;
    if (free_slots == 0) {
        spdlog::warn("[DecodeThread] Exact seek preview skipped: output buffer is full");
        return;
    }
    const size_t end = std::min(exact_seek_reorder_.size(),
                                selected + std::min(kExactSeekPreviewWindowFrames, free_slots));
    const size_t published = end - selected;
    bool conversion_failed = false;
    for (size_t i = selected; i < end; ++i) {
        if (!exact_seek_reorder_[i].frame) {
            continue;
        }
        if (i == selected && hw_enabled_ && hw_provider_) {
            hw_provider_->wait_idle();
            hw_visibility_flush_pending_ = false;
        } else {
            flush_hw_before_publish_if_needed(true);
        }
        TextureFrame tex_frame;
        if (i == selected &&
            exact_seek_reorder_[i].stable_frame.has_value() &&
            exact_seek_reorder_[i].stable_frame->texture_handle) {
            tex_frame = *exact_seek_reorder_[i].stable_frame;
        } else {
            auto converted = convert_frame_for_publish(exact_seek_reorder_[i].frame.get());
            if (!converted.has_value()) {
                spdlog::error("[DecodeThread] Frame conversion failed while publishing exact-seek window");
                output_buffer_.set_state(TrackState::Error);
                decode_paused_.store(true, std::memory_order_release);
                running_.store(false, std::memory_order_release);
                conversion_failed = true;
                break;
            }
            tex_frame = std::move(*converted);
        }
        output_buffer_.push_frame(std::move(tex_frame));
    }
    if (conversion_failed) {
        exact_seek_reorder_.clear();
        exact_seek_pending_frames_.clear();
        refresh_exact_seek_memory_stats();
        return;
    }
    exact_seek_pending_frames_.clear();
    for (size_t i = end; i < exact_seek_reorder_.size(); ++i) {
        exact_seek_pending_frames_.push_back(std::move(exact_seek_reorder_[i]));
    }
    output_buffer_.set_state(TrackState::Ready);
    if (pause_after_preroll_.load(std::memory_order_acquire)) {
        decode_paused_.store(true, std::memory_order_release);
    }
    post_seek_ = false;
    exact_seek_target_us_ = -1;
    drain_decoder_before_next_packet_ = true;
    exact_seek_reorder_.clear();
    refresh_exact_seek_memory_stats();
    spdlog::info("[DecodeThread] Exact seek drain: preview frame ready pts={:.3f}s, published={} frames, pending={} frames, state->Ready",
                 pts / 1e6, published, exact_seek_pending_frames_.size());
}

bool DecodeThread::publish_best_exact_seek_frame() {
    if (exact_seek_target_us_ < 0 || exact_seek_reorder_.empty()) {
        return false;
    }

    std::vector<int64_t> candidate_pts_us;
    candidate_pts_us.reserve(exact_seek_reorder_.size());
    for (const auto& candidate : exact_seek_reorder_) {
        candidate_pts_us.push_back(candidate.pts_us);
    }
    const auto selected = select_exact_seek_preview_index(
        candidate_pts_us, exact_seek_target_us_);
    if (!selected.has_value()) {
        return false;
    }

    const int64_t selected_pts = exact_seek_reorder_[*selected].pts_us;
    const size_t collected = exact_seek_reorder_.size();
    spdlog::info("[DecodeThread] Exact seek reorder: selected pts={:.3f}s from {} frames (target={:.3f}s)",
                 selected_pts / 1e6, collected, exact_seek_target_us_ / 1e6);
    publish_exact_seek_window(*selected);
    return true;
}

void DecodeThread::publish_pending_exact_seek_frames() {
    if (exact_seek_pending_frames_.empty()) {
        return;
    }
    auto candidate = std::move(exact_seek_pending_frames_.front());
    exact_seek_pending_frames_.pop_front();
    if (!candidate.frame) {
        refresh_exact_seek_memory_stats();
        return;
    }
    flush_hw_before_publish_if_needed(true);
    convert_and_push_frame(candidate.frame.get(), "pending exact-seek frame");
    refresh_exact_seek_memory_stats();
}

std::optional<TextureFrame> DecodeThread::convert_frame_for_publish(AVFrame* frame) {
    if (hw_enabled_ && !converter_.downloads_hardware_to_cpu()) {
        return converter_.snapshot_hardware_frame(frame);
    }
    return converter_.convert(frame);
}

bool DecodeThread::convert_and_push_frame(AVFrame* frame, const char* context) {
    auto converted = convert_frame_for_publish(frame);
    if (!converted.has_value()) {
        spdlog::error("[DecodeThread] Frame conversion failed ({})", context ? context : "unknown");
        output_buffer_.set_state(TrackState::Error);
        decode_paused_.store(true, std::memory_order_release);
        running_.store(false, std::memory_order_release);
        return false;
    }
    output_buffer_.push_frame(std::move(*converted));
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
        if (f->pkt_dts != AV_NOPTS_VALUE) {
            f->pkt_dts = av_rescale_q(f->pkt_dts, time_base_, {1, 1000000});
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
            exact_seek_pending_frames_.clear();
            refresh_exact_seek_memory_stats();
            drain_decoder_before_next_packet_ = false;

            // Always reset codec state on seek. During add-track initial seek,
            // demux can race ahead and the decoder may have already accepted
            // packets even though no frames have been published yet.
            safe_flush_codec();
            spdlog::info("[DecodeThread] Seek flush: codec buffers flushed (hw={})", hw_enabled_);

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

        if (should_publish_pending_exact_seek_frames(
                exact_seek_pending_frames_.size(),
                decode_paused_.load(std::memory_order_acquire),
                output_buffer_.state())) {
            publish_pending_exact_seek_frames();
            continue;
        }

        if (should_drain_decoder_before_next_packet(
                drain_decoder_before_next_packet_,
                decode_paused_.load(std::memory_order_acquire),
                output_buffer_.state())) {
            int drained = 0;
            while (true) {
                if (cancelled_.load(std::memory_order_acquire) ||
                    output_buffer_.state() == TrackState::Flushing) {
                    drain_decoder_before_next_packet_ = false;
                    break;
                }

                int ret = receive_codec_frame_seh_guarded(
                    codec_ctx_, frame, hw_enabled_, device_mutex_);
                if (codec_loop_is_seh_caught(ret)) {
                    output_buffer_.set_state(TrackState::Error);
                    decode_paused_.store(true, std::memory_order_release);
                    running_.store(false, std::memory_order_release);
                }

                if (codec_loop_is_again_or_eof(ret)) {
                    drain_decoder_before_next_packet_ = false;
                    break;
                }
                if (ret < 0) {
                    drain_decoder_before_next_packet_ = false;
                    break;
                }

                rescale_ts(frame);
                log_hw_frame_context_once(frame);
                flush_hw_before_publish_if_needed(true);
                if (!convert_and_push_frame(frame, "drain before next packet")) {
                    av_frame_unref(frame);
                    break;
                }
                av_frame_unref(frame);
                ++drained;

                if (decode_paused_.load(std::memory_order_acquire) ||
                    output_buffer_.state() == TrackState::Flushing) {
                    break;
                }
            }
            if (drained > 0) {
                perf_.frames_decoded.fetch_add(drained, std::memory_order_relaxed);
            }
            continue;
        }

        // Fully pause decode consumption so the packet queue preserves packets
        // and the demux thread stops at backpressure instead of racing to EOF.
        if (should_pause_decode_consumption(
                decode_paused_.load(std::memory_order_acquire),
                output_buffer_.state())) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Non-blocking pop with short sleep — allows seek_pending to be checked promptly
        PacketPopResult packet_result = input_queue_.try_pop();
        AVPacket* pkt = packet_result.packet;
        if (packet_result.status != PacketPopStatus::Packet || !pkt) {
            if (!running_.load(std::memory_order_acquire) ||
                cancelled_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            // EOF flush: drain codec once when the producer signals EOF.
            // Skip during Buffering (post-seek preroll) — the DemuxThread may
            // signal EOF very quickly after seek (file is cached), but we need
            // to keep decoding to fill the preroll buffer first.
            const auto eof_action = choose_eof_drain_action(
                input_queue_.is_eof(),
                eof_flushed_,
                output_buffer_.state(),
                exact_seek_target_us_ >= 0);
            if (eof_action != EofDrainAction::None) {
                if (eof_action == EofDrainAction::BufferingExactSeekDrain ||
                    eof_action == EofDrainAction::BufferingMarkFlushed) {
                    // Drain codec for exact seek to flush remaining DPB frames.
                    if (eof_action == EofDrainAction::BufferingExactSeekDrain) {
                        drain_codec(frame, rescale_ts, exact_seek_target_us_);
                        spdlog::info("[DecodeThread] Exact seek EOF drain: reorder buffer has {} frames",
                                     exact_seek_reorder_.size());
                    } else {
                        eof_flushed_ = true;
                    }

                    // Flush exact-seek reorder buffer at EOF — no more frames coming.
                    if (eof_action == EofDrainAction::BufferingExactSeekDrain) {
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
                } else if (eof_action == EofDrainAction::CodecDrain) {
                    int send_ret = send_codec_packet_seh_guarded(codec_ctx_, nullptr);
                    if (send_ret < 0 && send_ret != AVERROR(EAGAIN) && send_ret != AVERROR_EOF) {
                        if (codec_loop_is_seh_caught(send_ret)) {
                            output_buffer_.set_state(TrackState::Error);
                            decode_paused_.store(true, std::memory_order_release);
                            running_.store(false, std::memory_order_release);
                        }
                        break;
                    }
                    while (true) {
                        int ret = receive_codec_frame_seh_guarded(codec_ctx_, frame);
                        if (codec_loop_is_seh_caught(ret)) {
                            output_buffer_.set_state(TrackState::Error);
                            decode_paused_.store(true, std::memory_order_release);
                            running_.store(false, std::memory_order_release);
                            break;
                        }
                        if (codec_loop_is_again_or_eof(ret)) break;
                        if (ret < 0) break;

                        rescale_ts(frame);
                        log_hw_frame_context_once(frame);
                        flush_hw_before_publish_if_needed();
                        if (!convert_and_push_frame(frame, "EOF drain")) {
                            av_frame_unref(frame);
                            break;
                        }
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
        bool seek_pending = false;
        {
            std::lock_guard<std::mutex> seek_lock(seek_mutex_);
            if (seek_.pending) {
                seek_pending = true;
            }
        }
        if (should_discard_packet_before_decode(
                seek_pending,
                decode_paused_.load(std::memory_order_acquire),
                output_buffer_.state())) {
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
        ret = send_codec_packet_seh_guarded(codec_ctx_, pkt, hw_enabled_, device_mutex_);
        if (codec_loop_is_seh_caught(ret)) {
            av_packet_free(&pkt);
            output_buffer_.set_state(TrackState::Error);
            decode_paused_.store(true, std::memory_order_release);
            running_.store(false, std::memory_order_release);
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
            ret = receive_codec_frame_seh_guarded(codec_ctx_, frame, hw_enabled_, device_mutex_);
            if (codec_loop_is_seh_caught(ret)) {
                output_buffer_.set_state(TrackState::Error);
                decode_paused_.store(true, std::memory_order_release);
                running_.store(false, std::memory_order_release);
                break;
            }
            if (codec_loop_is_again_or_eof(ret)) {
                break;
            }
            if (ret < 0) {
                spdlog::error("[DecodeThread] Error receiving frame: {:#x}", static_cast<unsigned>(ret));
                break;
            }

            rescale_ts(frame);
            log_hw_frame_context_once(frame);

            // Exact seek: FFmpeg returns frames in display order after codec
            // reordering. Keep only the latest frame before target, then a
            // tiny preview window after it.
            if (exact_seek_target_us_ >= 0) {
                if (!should_collect_exact_seek_candidate(frame->pts, exact_seek_target_us_)) {
                    perf_.frames_dropped.fetch_add(1, std::memory_order_relaxed);
                    av_frame_unref(frame);
                    continue;
                }

                auto candidate = make_exact_seek_candidate(frame);
                ++frames_produced;
                if (candidate.frame) {
                    collect_exact_seek_candidate(std::move(candidate));
                }

                if (exact_seek_preview_window_ready()) {
                    publish_best_exact_seek_frame();
                    av_frame_unref(frame);
                    break;
                }

                av_frame_unref(frame);
                continue;
            }

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
            if (!convert_and_push_frame(frame, "decode loop")) {
                av_frame_unref(frame);
                break;
            }

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
            const auto publish_decision = choose_exact_seek_reorder_publish(
                exact_seek_target_us_ >= 0,
                exact_seek_reorder_.size(),
                input_queue_.is_eof(),
                input_queue_.size(),
                eof_flushed_,
                exact_seek_preview_window_ready());
            if (publish_decision.drain_codec) {
                drain_codec(frame, rescale_ts, exact_seek_target_us_);
                spdlog::info("[DecodeThread] Exact seek EOF: codec drain, reorder buffer now has {} frames",
                             exact_seek_reorder_.size());
            }

            if (publish_decision.publish) {
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
