#include "video_renderer/renderer.h"
#include "video_renderer/overlay/analysis_overlay_renderer.h"
#include "video_renderer/layout/layout_geometry.h"
#include "video_renderer/layout/layout_validation.h"
#include "video_renderer/renderer_config_validation.h"
#include "video_renderer/playback/renderer_playback_command_policy.h"
#include "video_renderer/seek/renderer_seek_log_policy.h"
#include "video_renderer/track/track_lifecycle.h"
#include "video_renderer/track/track_preroll_policy.h"
#include "video_renderer/track/track_present_policy.h"
#include "video_renderer/track/track_preview_policy.h"
#include "video_renderer/track/track_step_policy.h"
#include "audio/audio_output_factory.h"
#include "video_renderer/audio_coordinator.h"
#include "video_renderer/seek/seek_coordinator.h"
#include "video_renderer/render/device_loss_policy.h"
#include "video_renderer/render/presentation_backend_factory.h"
#include "video_renderer/render/presentation_snapshot.h"
#include "video_renderer/render/render_thread_platform.h"
#include "video_renderer/render/swap_chain_present_policy.h"
#include "video_renderer/track/track_snapshot.h"
#ifdef _WIN32
#include "video_renderer/capture/frame_capture_service.h"
#include "video_renderer/d3d11/render_backend.h"
#endif
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace vr {

// Max sleep for responsiveness (allows seek/pause within ~50 ms)
static constexpr int64_t MAX_SLEEP_US = 8000;  // 8ms cap → ~120Hz layout response
static constexpr auto kPausedHevcSeekSettleDelay = std::chrono::milliseconds(250);
static constexpr auto kStepForwardDecodeWait = std::chrono::milliseconds(180);

uint64_t elapsed_us_since(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
}

void stop_detached_track_pipeline(size_t slot, std::unique_ptr<TrackPipeline>& track) {
    if (!track) {
        return;
    }
    if (track->decode_thread) {
        spdlog::info("Renderer: stopping track[{}] decode ({})", slot, track->file_path);
        track->decode_thread->stop();
        spdlog::info("Renderer: track[{}] decode stopped", slot);
    }
    if (track->demux_thread) {
        spdlog::info("Renderer: stopping track[{}] demux ({})", slot, track->file_path);
        track->demux_thread->stop();
        spdlog::info("Renderer: track[{}] demux stopped", slot);
    }
    track.reset();
}

Renderer::Renderer()
    : owned_playback_(std::make_unique<PlaybackController>(create_default_audio_output))
    , playback_(owned_playback_.get())
    , audio_coordinator_(std::make_unique<AudioCoordinator>(*playback_))
    , seek_coordinator_(std::make_unique<SeekCoordinator>(kPausedHevcSeekSettleDelay))
    , analysis_overlay_renderer_(std::make_unique<AnalysisOverlayRenderer>()) {
#ifdef _WIN32
    frame_capture_ = new FrameCaptureService();
#endif
}

Renderer::Renderer(PlaybackController& playback)
    : playback_(&playback)
    , audio_coordinator_(std::make_unique<AudioCoordinator>(*playback_))
    , seek_coordinator_(std::make_unique<SeekCoordinator>(kPausedHevcSeekSettleDelay))
    , analysis_overlay_renderer_(std::make_unique<AnalysisOverlayRenderer>()) {
#ifdef _WIN32
    frame_capture_ = new FrameCaptureService();
#endif
}

Renderer::~Renderer() {
    shutdown();
#ifdef _WIN32
    delete frame_capture_;
    frame_capture_ = nullptr;
#endif
}

bool Renderer::initialize(const RendererConfig& config) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    const auto validation = validate_renderer_config(config);
    if (!validation.ok) {
        spdlog::error("Renderer: invalid config: {}", validation.message);
        return false;
    }

    // Flutter plugin configures logging before initialize().
    // Skip empty config to avoid clearing all sinks.
    if (!config.log_config.file_path.empty() || config.log_config.level != spdlog::level::info) {
        configure_logging(config.log_config);
    }

    // Crash handling is process-global. Hosts must opt in explicitly via the
    // windows_crash_handler module; Renderer initialization does not install hooks.

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (initialized_.load() || running_.load() || render_thread_.joinable()) {
        spdlog::warn("Renderer: initialize called while already initialized or running");
        return false;
    }

    auto fail = [this]() {
        release_resources_locked();
        return false;
    };

    hwnd_ = config.hwnd;
    headless_ = config.headless;
    target_width_ = config.width;
    target_height_ = config.height;
    shutting_down_.store(false, std::memory_order_release);
    device_state_.store(RendererDeviceState::Ready, std::memory_order_release);
    reset_d3d_metrics();
    playback_session_started_by_renderer_ = false;
    if (!playback_->audio_output()) {
        playback_->start_session();
        playback_session_started_by_renderer_ = true;
    }

    PresentationBackendConfig backend_config;
    backend_config.hwnd = hwnd_;
    backend_config.adapter = config.backend.adapter;
    backend_config.output = config.backend.output;
    backend_config.width = target_width_;
    backend_config.height = target_height_;
    backend_config.max_track_slots = config.backend.max_track_slots;
    backend_config.headless = config.headless;
    render_backend_kind_ = config.backend.type;
    const auto* backend_provider = config.backend.provider
        ? config.backend.provider
        : default_presentation_backend_provider();
    auto backend = backend_provider && backend_provider->supports(config.backend.type)
        ? backend_provider->create(config.backend.type)
        : nullptr;
    if (!backend) {
        spdlog::error("Renderer: unsupported presentation backend {}",
                      render_backend_kind_name(config.backend.type));
        return fail();
    }
    if (!backend->initialize(backend_config)) {
        return fail();
    }
    presentation_backend_ = std::move(backend);

    int next_initial_file_id = config.initial_file_id;
    const InitialTrackOpenHooks initial_track_hooks{
        [this](const std::string& path, bool use_hardware_decode) {
            return create_pipeline(path, use_hardware_decode);
        },
        [this, &next_initial_file_id]() {
            const int file_id = next_initial_file_id++;
            next_file_id_ = std::max(next_file_id_, file_id + 1);
            return file_id;
        },
        TrackPipelineStartHooks{
            [this](TrackPipeline& track) { configure_track_seek_callback(track); },
            [this](TrackPipeline& track) { configure_track_error_callback(track); },
            [this](TrackPipeline& track) { register_track_audio(track); },
            [this](int file_id) { unregister_track_audio(file_id); },
        },
    };
    open_initial_track_pipelines(
        tracks_, config.video_paths, config.use_hardware_decode,
        initial_track_hooks, "Renderer");
    assign_missing_track_generations_locked();

    if (!tracks_.has_active_tracks()) {
        spdlog::error("Renderer: no valid tracks");
        return fail();
    }

    layout_controller_.reset(layout_);
    layout_controller_.append_tracks(layout_, tracks_);

    // Setup render sink
    render_sink_ = std::make_unique<RenderSink>(playback_->clock());
    bind_existing_tracks_to_render_sink(tracks_, *render_sink_);

    // Cache duration (immutable until tracks are added/removed)
    cached_duration_us_ = compute_track_duration_cache(tracks_);

    initialized_ = true;

    perf_baseline_tracker_.reset(std::chrono::steady_clock::now());

    // Start render loop immediately (paused mode).
    // Decodes and displays first frame, fills buffers, but does not advance playback.
    running_ = true;
    try {
        render_thread_ = std::thread(&Renderer::render_loop, this);
    } catch (const std::exception& e) {
        spdlog::error("Renderer: failed to start render thread: {}", e.what());
        running_ = false;
        initialized_ = false;
        return fail();
    }

    spdlog::info("Renderer: initialized with {} tracks", tracks_.count());
    return true;
}

void Renderer::shutdown() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    bool has_resources = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        has_resources = has_resources_locked();
        if (!has_resources) {
            clear_event_callback();
            return;
        }

        shutting_down_.store(true, std::memory_order_release);
        running_ = false;
        playing_ = false;
    }

    spdlog::info("Renderer: shutdown begin");

    if (render_thread_.joinable()) {
        spdlog::info("Renderer: waiting for render thread join");
        render_thread_.join();
        spdlog::info("Renderer: render thread joined");
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        release_resources_locked();
    }

    spdlog::info("Renderer: shutdown complete");
}

bool Renderer::has_resources_locked() const {
    return tracks_.has_active_tracks() ||
           presentation_backend_ ||
           render_sink_ ||
           initialized_.load() ||
           running_.load() ||
           render_thread_.joinable();
}

void Renderer::release_resources_locked() {
    running_ = false;
    playing_ = false;
    clear_event_callback();

    // Clear cached frames that may hold hw decode surface references.
    // Must happen before decode_thread->stop() frees hw_device_ctx,
    // otherwise hw_frame_ref cleanup will access a freed device context.
    last_decision_ = PresentDecision();

    tracks_.stop_all([this](size_t, TrackPipeline& track) {
        unregister_track_audio(track.file_id);
    });

    render_sink_.reset();
    if (playback_ && playback_session_started_by_renderer_) {
        playback_->stop_session();
        playback_session_started_by_renderer_ = false;
    }
    if (presentation_backend_) {
        presentation_backend_->shutdown();
        presentation_backend_.reset();
    }

    hwnd_ = nullptr;
    headless_ = false;
    target_width_ = 1920;
    target_height_ = 1080;
    cached_duration_us_ = 0;
    next_file_id_ = 1;
    next_track_generation_ = 1;
    layout_controller_.reset(layout_);
    if (analysis_overlay_renderer_) {
        analysis_overlay_renderer_->reset();
    }
    preview_drawn_ = false;
    was_buffering_ = false;
    if (seek_coordinator_) {
        seek_coordinator_->reset();
    }
    loop_range_ = LoopRangeState();
    pending_width_.store(0);
    pending_height_.store(0);
    render_loop_controller_.reset();
    perf_baseline_tracker_.reset();
    initialized_ = false;
    device_state_.store(RendererDeviceState::Ready, std::memory_order_release);
}

void Renderer::reset_d3d_metrics() {
    d3d_metrics_.render_wait_us.store(0, std::memory_order_relaxed);
    d3d_metrics_.render_wait_count.store(0, std::memory_order_relaxed);
    d3d_metrics_.frame_copy_us.store(0, std::memory_order_relaxed);
    d3d_metrics_.frame_copy_count.store(0, std::memory_order_relaxed);
    d3d_metrics_.present_publish_us.store(0, std::memory_order_relaxed);
    d3d_metrics_.present_publish_count.store(0, std::memory_order_relaxed);
    d3d_metrics_.shared_texture_resize_count.store(0, std::memory_order_relaxed);
    d3d_metrics_.device_lost_count.store(0, std::memory_order_relaxed);
    d3d_metrics_.texture_sharing_failure_count.store(0, std::memory_order_relaxed);
}

std::function<void(const char*)> Renderer::frame_failure_callback_snapshot() const {
    return frame_failure_callback_;
}

std::string Renderer::presentation_backend_last_error() const {
    if (!presentation_backend_) {
        return "presentation backend is not available";
    }
    const char* error = presentation_backend_->last_error();
    return error && error[0] != '\0' ? error : "presentation backend draw failed";
}

void Renderer::assign_missing_track_generations_locked() {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i] && tracks_[i]->generation == 0) {
            tracks_[i]->generation = next_track_generation_++;
        }
    }
}

D3D11RenderBackend* Renderer::d3d_backend() const {
#ifdef _WIN32
    if (!presentation_backend_ ||
        presentation_backend_->kind() != PresentationBackendKind::D3D11) {
        return nullptr;
    }
    return static_cast<D3D11RenderBackend*>(presentation_backend_.get());
#else
    return nullptr;
#endif
}

D3D11Device* Renderer::d3d_device() const {
#ifdef _WIN32
    auto* backend = d3d_backend();
    return backend ? backend->device() : nullptr;
#else
    return nullptr;
#endif
}

D3D11FramePresenter* Renderer::frame_presenter() const {
#ifdef _WIN32
    auto* backend = d3d_backend();
    return backend ? backend->frame_presenter() : nullptr;
#else
    return nullptr;
#endif
}

D3D11HeadlessOutput* Renderer::headless_output() const {
#ifdef _WIN32
    auto* backend = d3d_backend();
    return backend ? backend->headless_output() : nullptr;
#else
    return nullptr;
#endif
}

D3D11RenderResources* Renderer::d3d_resources() const {
#ifdef _WIN32
    auto* backend = d3d_backend();
    return backend ? backend->resources() : nullptr;
#else
    return nullptr;
#endif
}

void Renderer::play() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto plan = plan_renderer_play_command(initialized_, playing_);
    if (!plan.execute) return;
    if (plan.reset_seek && seek_coordinator_) {
        seek_coordinator_->reset();
    }

    apply_playback_decode_state_locked(plan.playback_active);
    if (plan.play_clock) {
        playback_->play();
    }
    playing_ = plan.playing;
}

void Renderer::pause() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto plan = plan_renderer_pause_command();
    apply_playback_decode_state_locked(plan.playback_active);
    if (plan.pause_clock) {
        playback_->pause();
    }
    playing_ = plan.playing;
}

void Renderer::seek(int64_t target_pts_us, SeekType type, int64_t request_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (request_id >= 0) {
        pending_seek_event_request_id_ = request_id;
        pending_seek_event_target_pts_us_ = target_pts_us;
        pending_seek_event_emitted_ = false;
    }
    seek_internal(lock, target_pts_us, type);
}

void Renderer::set_loop_range(bool enabled, int64_t start_us, int64_t end_us) {
    const auto validation = validate_loop_range(enabled, start_us, end_us);
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid loop range: {}", validation.message);
        return;
    }

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto next = normalize_loop_range_state(enabled, start_us, end_us);
    if (loop_range_states_equal(loop_range_, next)) {
        return;
    }

    loop_range_ = next;
    spdlog::debug("[Renderer] loop range {}: {:.3f}s -> {:.3f}s",
                  next.enabled ? "enabled" : "disabled",
                  next.start_us / 1e6,
                  next.end_us / 1e6);
}

void Renderer::set_audible_track(int file_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!audio_coordinator_) return;
    if (file_id >= 0 && find_slot_by_file_id(file_id) < 0) {
        file_id = -1;
    }
    audio_coordinator_->set_active_track(file_id);
}

int Renderer::audible_track() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return audio_coordinator_ ? audio_coordinator_->active_track() : -1;
}

bool Renderer::has_audio() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const int slot = first_active_track();
    if (slot < 0 || !tracks_[static_cast<size_t>(slot)] ||
        !tracks_[static_cast<size_t>(slot)]->demux_thread) {
        return false;
    }
    const auto& stats = tracks_[static_cast<size_t>(slot)]->demux_thread->stats();
    return stats.audio_stream_index >= 0 && stats.audio_codec_params != nullptr;
}

int Renderer::audio_sample_rate() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const int slot = first_active_track();
    if (slot < 0 || !tracks_[static_cast<size_t>(slot)] ||
        !tracks_[static_cast<size_t>(slot)]->demux_thread) {
        return 0;
    }
    return tracks_[static_cast<size_t>(slot)]->demux_thread->stats().sample_rate;
}

int Renderer::audio_channels() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    const int slot = first_active_track();
    if (slot < 0 || !tracks_[static_cast<size_t>(slot)] ||
        !tracks_[static_cast<size_t>(slot)]->demux_thread) {
        return 0;
    }
    return tracks_[static_cast<size_t>(slot)]->demux_thread->stats().channels;
}

void Renderer::seek_internal(std::unique_lock<std::mutex>& state_lock,
                             int64_t target_pts_us,
                             SeekType type,
                             bool allow_deferred,
                             bool force_recreate_paused_hevc) {
    // Caller must hold lifecycle_mutex_ and state_mutex_.
    // See native/docs/SEEK_STRATEGY.md for codec/container-specific exact seek
    // limits, especially H.264 FLV streams without repeated SPS/PPS on IDR.
    if (!state_lock.owns_lock()) {
        spdlog::error("[Renderer] seek_internal called without state lock");
        return;
    }
    const PendingSeekPreviewEventState pending_event{
        pending_seek_event_request_id_ >= 0,
        pending_seek_event_emitted_,
        pending_seek_event_target_pts_us_,
    };
    const auto seek_target = resolve_seek_target(
        target_pts_us, effective_duration_us_locked(), pending_event);
    target_pts_us = seek_target.target_pts_us;
    const auto clamp_log = build_seek_clamp_log_facts(seek_target);
    if (clamp_log.should_log) {
        spdlog::info("[Renderer] seek_internal clamp: requested={:.3f}s, clamped={:.3f}s, duration={:.3f}s",
                     clamp_log.requested_seconds,
                     clamp_log.clamped_seconds,
                     clamp_log.duration_seconds);
    }
    if (seek_target.retarget_pending_event) {
        pending_seek_event_target_pts_us_ = target_pts_us;
    }
    const auto request_log = build_seek_request_log_facts(target_pts_us, type);
    spdlog::info("[Renderer] seek_internal: target={:.3f}s, type={}",
                 request_log.target_seconds,
                 request_log.type_label);
    RendererSeekClockGateInput clock_gate_input;
    clock_gate_input.allow_deferred = allow_deferred;
    clock_gate_input.playing = playing_.load();
    clock_gate_input.has_hevc_hw_track =
        allow_deferred ? has_hevc_hw_track_locked() : false;
    clock_gate_input.target_pts_us = target_pts_us;
    clock_gate_input.type = type;
    const auto clock_gate = plan_renderer_seek_clock_gate(clock_gate_input);
    if (clock_gate.seek_clock) {
        playback_->seek_clock(clock_gate.target_pts_us);
    }
    if (should_defer_paused_hevc_seek_locked(clock_gate)) {
        return;
    }

    bool applied_seek = false;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        const TrackSeekSlotApplicationHooks seek_hooks{
            TrackSeekPreparationHooks{
                [this](int file_id, bool paused) {
                    if (audio_coordinator_) {
                        audio_coordinator_->set_track_decode_paused(file_id, paused);
                    }
                },
                [this, i]() {
                    if (presentation_backend_) {
                        presentation_backend_->reset_track(i);
                    }
                },
            },
            [this, &state_lock](size_t slot, int64_t seek_target_us, SeekType seek_type) {
                return recreate_pipeline_for_seek(
                    state_lock, slot, seek_target_us, seek_type);
            },
        };
        const auto seek_result = apply_track_seek_to_slot(
            tracks_, i, target_pts_us, type, playing_.load(),
            force_recreate_paused_hevc, seek_hooks);
        if (!seek_result.slot_present) {
            continue;
        }
        const auto& seek_facts = seek_result.facts;
        if (seek_facts.warn_h264_flv_exact_seek) {
            spdlog::warn("[Renderer] Exact seek on H.264/FLV is best-effort: "
                         "streams that omit SPS/PPS on IDR frames can decode "
                         "incorrectly after seek. Remux/re-encode with repeated "
                         "headers for frame-accurate previews.");
        }
        const auto& track_target = seek_facts.target;
        const auto track_clamp_log =
            build_track_seek_target_clamp_log_facts(i, track_target);
        if (track_clamp_log.should_log) {
            spdlog::info("[Renderer] seek_internal: track[{}] target clamp "
                         "requested={:.3f}s, clamped={:.3f}s",
                         track_clamp_log.slot,
                         track_clamp_log.requested_seconds,
                         track_clamp_log.clamped_seconds);
        }
        const auto& seek_prep = seek_result.preparation;
        const auto& seek_execution = seek_result.execution;
        const auto coalescing_log = build_track_seek_coalescing_log_facts(
            i, track_target, seek_prep, seek_execution);
        if (coalescing_log.should_log) {
            spdlog::info("[Renderer] seek_internal: track[{}] coalescing HEVC HW seek during transition "
                         "(buf_state_before={}, target={:.3f}s)",
                         coalescing_log.slot,
                         coalescing_log.buffer_state_before,
                         coalescing_log.target_seconds);
        }
        if (!seek_execution.applied_seek) {
            continue;
        }
        auto* track = tracks_[i].get();
        if (!track) {
            continue;
        }
        applied_seek = true;
        const auto cleared_log = build_track_seek_cleared_log_facts(
            i,
            track_target,
            seek_prep,
            seek_execution,
            track->track_buffer->total_count());
        spdlog::info("[Renderer] seek_internal: track[{}] cleared (buf={}->{}, pq={}->0), state->Flushing, target={:.3f}s",
                     cleared_log.slot,
                     cleared_log.buffered_frames_before,
                     cleared_log.buffered_frames_after,
                     cleared_log.packet_queue_size_before,
                     cleared_log.target_seconds);
    }
    if (applied_seek) {
        preview_drawn_ = false;
        last_decision_ = PresentDecision();
    }
}

bool Renderer::should_defer_paused_hevc_seek_locked(const RendererSeekClockGatePlan& gate) {
    if (!gate.evaluate_paused_hevc_defer || !seek_coordinator_) {
        return false;
    }

    const bool deferred = seek_coordinator_->should_defer_paused_hevc_seek(
        gate.playing, gate.has_hevc_hw_track, gate.target_pts_us, gate.type);
    if (deferred) {
        spdlog::info("[Renderer] Deferring paused HEVC HW seek to {:.3f}s",
                     gate.target_pts_us / 1e6);
    }
    return deferred;
}

bool Renderer::apply_deferred_paused_hevc_seek_locked(
    std::unique_lock<std::mutex>& state_lock) {
    if (!seek_coordinator_) {
        return false;
    }

    const auto deferred = seek_coordinator_->take_deferred_paused_hevc_seek(playing_.load());
    if (!deferred.has_value()) {
        return false;
    }
    spdlog::info("[Renderer] Applying deferred paused HEVC HW seek to {:.3f}s",
                 deferred->target_pts_us / 1e6);
    seek_internal(state_lock, deferred->target_pts_us, deferred->type, false, true);
    return true;
}

bool Renderer::apply_loop_range_locked(std::unique_lock<std::mutex>& state_lock) {
    const int64_t pts = playback_->clock().current_pts_us();
    LoopRangeSeekInput input;
    input.playing = playing_.load();
    input.loop_enabled = loop_range_.enabled;
    input.clock_paused = playback_->clock().is_paused();
    input.current_pts_us = pts;
    input.start_us = loop_range_.start_us;
    input.end_us = loop_range_.end_us;
    const auto decision = choose_loop_range_seek(input);
    if (!decision.should_seek) {
        return false;
    }

    spdlog::info("[Renderer] loop range boundary: pts={:.3f}s, seeking to {:.3f}s",
                 pts / 1e6, decision.target_pts_us / 1e6);
    seek_internal(state_lock, decision.target_pts_us, SeekType::Exact);
    return true;
}

void Renderer::mark_paused_hevc_seek_preview_drawn_locked() {
    if (!seek_coordinator_) {
        return;
    }
    const bool was_in_flight = seek_coordinator_->paused_hevc_seek_in_flight();
    seek_coordinator_->mark_paused_hevc_preview_drawn(has_hevc_hw_track_locked());
    if (was_in_flight && !seek_coordinator_->paused_hevc_seek_in_flight()) {
        spdlog::info("[Renderer] Paused HEVC HW seek preview ready, settle window {}ms",
                     static_cast<long long>(kPausedHevcSeekSettleDelay.count()));
    }
}

bool Renderer::has_hevc_hw_track_locked() const {
    return any_track_uses_hardware_codec(tracks_, AV_CODEC_ID_HEVC);
}

void Renderer::emit_event(const RendererEvent& event) {
    if (shutting_down_.load(std::memory_order_acquire)) {
        return;
    }

    RendererEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(event_callback_mutex_);
        if (shutting_down_.load(std::memory_order_acquire)) {
            return;
        }
        callback = event_callback_;
    }
    if (callback) {
        if (event.type == RendererEvent::Type::SeekPreviewPresented) {
            spdlog::info("[Renderer] emit seekPreviewPresented request_id={} file_id={} pts={:.3f}s dts={:.3f}s",
                         event.request_id,
                         event.track_file_id,
                         event.pts_us / 1e6,
                         event.dts_us == kNoTimestampUs ? -1.0 : event.dts_us / 1e6);
        } else if (event.type == RendererEvent::Type::TrackError) {
            spdlog::error("[Renderer] emit trackError file_id={} error_code={:#x}",
                          event.track_file_id,
                          static_cast<unsigned>(event.error_code));
        }
        callback(event);
    }
}

void Renderer::clear_event_callback() {
    std::lock_guard<std::mutex> lock(event_callback_mutex_);
    event_callback_ = {};
}

bool Renderer::has_event_callback_for_test() const {
    std::lock_guard<std::mutex> lock(event_callback_mutex_);
    return static_cast<bool>(event_callback_);
}

void Renderer::enter_terminal_render_loop_error_for_test(const char* reason) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    enter_terminal_render_loop_error_locked(reason);
}

void Renderer::emit_seek_preview_presented_events(const PresentDecision& decision) {
    int64_t request_id = -1;
    int64_t target_pts_us = -1;
    std::vector<SeekPreviewPresentedTrackEvent> events;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (pending_seek_event_request_id_ < 0 || pending_seek_event_emitted_) {
            return;
        }
        request_id = pending_seek_event_request_id_;
        target_pts_us = pending_seek_event_target_pts_us_;
        pending_seek_event_emitted_ = true;
        events = collect_seek_preview_presented_track_events(
            tracks_, decision, request_id, target_pts_us);
    }

    for (const auto& track_event : events) {
        RendererEvent event;
        event.type = RendererEvent::Type::SeekPreviewPresented;
        event.request_id = track_event.request_id;
        event.track_file_id = track_event.file_id;
        event.pts_us = track_event.pts_us;
        event.dts_us = track_event.dts_us;
        event.target_pts_us = track_event.target_pts_us;
        emit_event(event);
    }
}

void Renderer::update_track_geometry_from_decision_locked(const PresentDecision& decision) {
    const auto updates = update_layout_track_geometry_from_decision(tracks_, decision);
    for (const auto& update : updates) {
        spdlog::info(
            "[Renderer] track[{}] display geometry changed: {}x{} aspect={:.6f} -> {}x{} aspect={:.6f}",
            update.slot,
            update.old_width,
            update.old_height,
            update.old_aspect,
            update.new_width,
            update.new_height,
            update.new_aspect);
    }
}

void Renderer::step_forward() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    PresentDecision step_decision;
    bool have_step_decision = false;
    bool need_decode_wait = false;
    bool need_exact_seek = false;
    int64_t exact_seek_target = 0;
    StepForwardDecisionApplication step_application;
    const auto build_step_decision_locked = [this](PresentDecision& decision) {
        return build_step_forward_decision(
            tracks_,
            playback_->clock().current_pts_us(),
            compute_min_current_frame_duration_us(tracks_),
            last_decision_,
            decision);
    };
    const auto apply_step_decision_locked =
        [this](const PresentDecision& decision) {
            return apply_step_forward_decision(
                tracks_,
                playback_->clock().current_pts_us(),
                decision,
                last_decision_);
    };
    const auto set_video_decode_paused_locked = [this](bool paused) {
        apply_track_video_decode_pause_state(
            tracks_,
            paused,
            [](size_t, TrackPipeline& track, bool paused) {
                track.decode_thread->set_decode_paused(paused);
            });
    };

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto step_plan = plan_renderer_step_command(
            initialized_,
            initialized_ && has_buffering_track(tracks_));
        if (!step_plan.execute) return;

        if (step_plan.pause_clock) {
            playback_->clock().pause();
        }
        playing_ = step_plan.playing;

        if (build_step_decision_locked(step_decision)) {
            step_application = apply_step_decision_locked(step_decision);
            if (step_application.has_clock_target) {
                playback_->clock().seek(step_application.clock_target_us);
            }
            have_step_decision = true;
        } else {
            discard_step_forward_consumed_frames(
                tracks_,
                playback_->clock().current_pts_us(),
                last_decision_,
                last_decision_);
            set_video_decode_paused_locked(false);
            need_decode_wait = true;
        }
    }

    if (need_decode_wait) {
        const auto deadline = std::chrono::steady_clock::now() + kStepForwardDecodeWait;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!initialized_) return;
                if (build_step_decision_locked(step_decision)) {
                    set_video_decode_paused_locked(true);
                    step_application = apply_step_decision_locked(step_decision);
                    if (step_application.has_clock_target) {
                        playback_->clock().seek(step_application.clock_target_us);
                    }
                    have_step_decision = true;
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        if (!have_step_decision) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (!initialized_) return;
            set_video_decode_paused_locked(true);

            const auto fallback_seek = choose_step_forward_exact_seek_target(
                tracks_,
                playback_->clock().current_pts_us(),
                cached_duration_us_,
                last_decision_);
            exact_seek_target = fallback_seek.target_pts_us;
            spdlog::info("[Renderer] step_forward exact_seek: visible_pts={:.3f}s, clock_pts={:.3f}s, duration={:.3f}ms, target={:.3f}s",
                         fallback_seek.base_pts_us / 1e6,
                         fallback_seek.clock_pts_us / 1e6,
                         fallback_seek.frame_duration_us / 1e3,
                         exact_seek_target / 1e6);
            need_exact_seek = true;
        }
    }

    if (have_step_decision) {
        present_frame(step_decision);
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            last_decision_ = step_decision;
        }
        double pts = step_application.has_clock_target
                     ? step_application.presented_pts_us / 1e6 : -1.0;
        spdlog::info("[Renderer] draw_paused_frame(step_forward): pts={:.3f}s", pts);
        return;
    }

    if (need_exact_seek) {
        std::unique_lock<std::mutex> lock(state_mutex_);
        seek_internal(lock, exact_seek_target, SeekType::ExactStepForward);
        spdlog::info("[Renderer] step_forward exact_seek done: clock_pts={:.3f}s",
                     playback_->clock().current_pts_us() / 1e6);
    }
}

void Renderer::step_backward() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::unique_lock<std::mutex> lock(state_mutex_);
        const auto step_plan = plan_renderer_step_command(
            initialized_,
            initialized_ && has_buffering_track(tracks_));
        if (!step_plan.execute) return;

        if (step_plan.pause_clock) {
            playback_->clock().pause();
        }
        playing_ = step_plan.playing;

        if (retreat_tracks_if_all_can_retreat(tracks_)) {
            const auto retreat_application =
                choose_step_backward_retreat_application(tracks_);
            if (retreat_application.has_clock_target) {
                playback_->clock().seek(retreat_application.clock_target_us);
            }
        } else {
            // Cache miss: exact seek to (current_pts - frame_duration - margin)
            // Add 1ms margin: frame duration is integer-truncated (e.g. 1/60s → 16666us)
            // but actual PTS spacing is 16667us, so (pts - dur) overshoots the
            // previous frame by 1us and exact seek's "< target" check discards it.
            const auto fallback_seek = choose_step_backward_exact_seek_target(
                tracks_,
                playback_->clock().current_pts_us());
            const int64_t target = fallback_seek.target_pts_us;
            spdlog::info("[Renderer] step_backward exact_seek: pts={:.3f}s, duration={:.3f}ms, target={:.3f}s",
                         fallback_seek.clock_pts_us / 1e6,
                         fallback_seek.frame_duration_us / 1e3,
                         target / 1e6);
            seek_internal(lock, target, SeekType::Exact);
            spdlog::info("[Renderer] step_backward exact_seek done: clock_pts={:.3f}s",
                         playback_->clock().current_pts_us() / 1e6);
            // Don't draw stale frame — seek_internal set preview_drawn_=false,
            // render loop will draw the new frame when decode completes.
            return;
        }
    }
    draw_paused_frame("step_backward");
}

bool Renderer::draw_paused_frame(const char* reason) {
    PresentDecision decision;
    bool has_frame = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto snapshot = build_available_paused_frame_snapshot(tracks_);
        decision = snapshot.decision;
        has_frame = snapshot.has_frame;
        filter_present_decision_against_tracks(last_decision_, tracks_);
        if (!has_frame && present_decision_has_frame(last_decision_)) {
            decision = last_decision_;
            has_frame = true;
        }
    }
    if (!has_frame) {
        return false;
    }
    present_frame(decision);
    int ref = -1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        filter_present_decision_against_tracks(decision, tracks_);
        last_decision_ = decision;
        ref = first_active_track();
    }
    double pts = (ref >= 0 && decision.frames[ref].has_value())
                 ? decision.frames[ref]->pts_us / 1e6 : -1.0;
    spdlog::info("[Renderer] draw_paused_frame({}): pts={:.3f}s", reason, pts);
    return true;
}

bool Renderer::request_frame_refresh(const char* reason) {
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!initialized_.load(std::memory_order_acquire) ||
            shutting_down_.load(std::memory_order_acquire)) {
            return false;
        }
    }
    const char* refresh_reason = reason && reason[0] != '\0'
                                     ? reason
                                     : "request_frame_refresh";
    if (playing_.load(std::memory_order_acquire)) {
        redraw_layout();
        return true;
    }
    return draw_paused_frame(refresh_reason);
}

RendererDrawSnapshot Renderer::build_draw_snapshot_locked(
    const PresentDecision& decision) const {
    RendererDrawSnapshot snapshot;
    snapshot.decision = decision;
    filter_present_decision_against_tracks(snapshot.decision, tracks_);
    snapshot.layout = layout_;
    snapshot.track_geometry = snapshot_layout_track_geometry(tracks_);
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) {
            continue;
        }
        auto& out = snapshot.tracks[i];
        out.active = true;
        out.file_id = tracks_[i]->file_id;
        out.generation = tracks_[i]->generation;
        out.offset_us = tracks_[i]->offset_us;
        out.video_width = tracks_[i]->video_width;
        out.video_height = tracks_[i]->video_height;
        out.video_aspect = tracks_[i]->video_aspect;
    }
    for (int i = 0; i < 4; ++i) {
        snapshot.background_color[i] = background_color_[i];
    }
    snapshot.target_width = target_width_;
    snapshot.target_height = target_height_;
    return snapshot;
}

void Renderer::wait_gpu_idle(const char* label) {
    const auto start = std::chrono::steady_clock::now();
    if (presentation_backend_) {
        presentation_backend_->wait_idle(label);
    }
    d3d_metrics_.render_wait_us.fetch_add(elapsed_us_since(start), std::memory_order_relaxed);
    d3d_metrics_.render_wait_count.fetch_add(1, std::memory_order_relaxed);
}

bool Renderer::draw_headless_and_publish(const RendererDrawSnapshot& snapshot,
                                         const char* label,
                                         std::function<void()>& callback) {
#ifdef _WIN32
    callback = {};
    if (shutting_down_.load(std::memory_order_acquire)) {
        return false;
    }
    auto* output = headless_output();
    auto* resources = d3d_resources();
    if (!output || !resources) {
        return false;
    }
    {
        std::lock_guard<std::mutex> tex_lock(texture_mutex());
        auto* rtv = output->begin_frame_locked();
        if (!rtv) {
            return false;
        }
        resources->cached_rtv = rtv;
    }
    if (!draw_frame(snapshot)) {
        return false;
    }
    const auto publish_start = std::chrono::steady_clock::now();
    output->wait_gpu_idle(label);
    {
        std::lock_guard<std::mutex> tex_lock(texture_mutex());
        callback = output->publish_frame_locked();
    }
    if (shutting_down_.load(std::memory_order_acquire)) {
        callback = {};
    }
    d3d_metrics_.present_publish_us.fetch_add(
        elapsed_us_since(publish_start), std::memory_order_relaxed);
    d3d_metrics_.present_publish_count.fetch_add(1, std::memory_order_relaxed);
    return true;
#else
    (void)snapshot;
    (void)label;
    callback = {};
    return false;
#endif
}

void Renderer::enter_terminal_device_lost_locked(const char* operation) {
    const auto plan = plan_renderer_device_lost_transition(
        device_state_.load(std::memory_order_acquire));
    if (!plan.apply) {
        return;
    }

    device_state_.store(plan.pre_terminal_state, std::memory_order_release);
    const long reason = presentation_backend_
        ? presentation_backend_->device_removed_reason()
        : 0;
    if (plan.count_device_lost) {
        d3d_metrics_.device_lost_count.fetch_add(1, std::memory_order_relaxed);
    }
    spdlog::error(
        "[Renderer] D3D11 device lost during {}; entering terminal renderer state "
        "(reason={:#x})",
        operation,
        static_cast<unsigned long>(reason));

    running_ = false;
    playing_ = false;
    if (plan.pause_playback) {
        playback_->pause();
    }
    if (plan.pause_decode) {
        set_decode_paused_for_all_tracks(true);
    }
    if (plan.clear_initialized) {
        initialized_ = false;
    }
    device_state_.store(plan.final_state, std::memory_order_release);
}

void Renderer::enter_terminal_render_loop_error_locked(const char* reason) {
    const auto plan = plan_renderer_runtime_error_transition(
        device_state_.load(std::memory_order_acquire));
    if (!plan.apply) {
        return;
    }
    device_state_.store(plan.pre_terminal_state, std::memory_order_release);
    spdlog::error(
        "[Renderer] Render loop entered terminal runtime state after {}",
        reason);
    running_ = false;
    playing_ = false;
    if (plan.clear_initialized) {
        initialized_ = false;
    }
    if (plan.pause_playback) {
        playback_->pause();
    }
    if (plan.pause_decode) {
        set_decode_paused_for_all_tracks(true);
    }
    device_state_.store(plan.final_state, std::memory_order_release);
}

void Renderer::present_frame(const PresentDecision& decision) {
    RendererDrawSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        spdlog::debug("[present_frame] mode={}", layout_.mode);
        PresentDecision filtered_decision = decision;
        filter_present_decision_against_tracks(filtered_decision, tracks_);
        update_track_geometry_from_decision_locked(filtered_decision);
        snapshot = build_draw_snapshot_locked(filtered_decision);
    }
    std::function<void()> frame_callback;
    auto frame_failure_callback = frame_failure_callback_snapshot();
    std::string frame_failure_error;
    const bool attempted_draw = present_decision_has_frame(snapshot.decision);
    bool device_lost = false;
    bool drew = false;
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        auto* backend = presentation_backend_.get();
        if (headless_) {
            if (backend && backend->renderer_manages_headless_publish()) {
                drew = draw_headless_and_publish(snapshot, "present_frame", frame_callback);
            } else {
                drew = draw_frame(snapshot);
                if (drew) {
                    frame_callback = frame_callback_;
                }
            }
            device_lost = backend && backend->poll_device_removed("headless present");
        } else {
            drew = draw_frame(snapshot);
            if (should_present_swap_chain_after_draw(
                    drew, backend && backend->supports_swap_chain_present())) {
                const auto present_start = std::chrono::steady_clock::now();
                const bool presented = backend->present_swap_chain(0);
                d3d_metrics_.present_publish_us.fetch_add(
                    elapsed_us_since(present_start), std::memory_order_relaxed);
                d3d_metrics_.present_publish_count.fetch_add(1, std::memory_order_relaxed);
                device_lost = !presented && backend->device_lost();
            } else {
                device_lost = backend && backend->device_lost();
            }
        }
        if (attempted_draw && !drew) {
            frame_failure_error = presentation_backend_last_error();
        }
    }
    if (device_lost) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_device_lost_locked("present_frame");
        return;
    }
    if (drew) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        preview_drawn_ = true;
    }
    if (frame_callback && !shutting_down_.load(std::memory_order_acquire)) {
        frame_callback();
    }
    if (attempted_draw && !drew && frame_failure_callback &&
        !shutting_down_.load(std::memory_order_acquire)) {
        frame_failure_callback(frame_failure_error.c_str());
    }
}

void Renderer::redraw_layout() {
    RendererDrawSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot = build_draw_snapshot_locked(last_decision_);
    }
    std::function<void()> frame_callback;
    auto frame_failure_callback = frame_failure_callback_snapshot();
    std::string frame_failure_error;
    const bool attempted_draw = present_decision_has_frame(snapshot.decision);
    bool device_lost = false;
    bool drew = false;
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        auto* backend = presentation_backend_.get();
        if (headless_) {
            if (backend && backend->renderer_manages_headless_publish()) {
                drew = draw_headless_and_publish(snapshot, "redraw_layout", frame_callback);
            } else {
                drew = draw_frame(snapshot);
                if (drew) {
                    frame_callback = frame_callback_;
                }
            }
            device_lost = backend && backend->poll_device_removed("headless redraw");
        } else if (backend) {
            drew = draw_frame(snapshot);
            backend->wait_idle("redraw_layout");
            device_lost = backend->poll_device_removed("redraw_layout");
        }
        if (attempted_draw && !drew) {
            frame_failure_error = presentation_backend_last_error();
        }
    }
    if (device_lost) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_device_lost_locked("redraw_layout");
        return;
    }
    if (drew) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        preview_drawn_ = true;
    }
    if (frame_callback && !shutting_down_.load(std::memory_order_acquire)) {
        frame_callback();
    }
    if (attempted_draw && !drew && frame_failure_callback &&
        !shutting_down_.load(std::memory_order_acquire)) {
        frame_failure_callback(frame_failure_error.c_str());
    }
}

bool Renderer::capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    auto* output = headless_output();
    if (!headless_ || !output) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    if (!frame_capture_) {
        return false;
    }
    return frame_capture_->capture_headless_front_buffer(
        *output, device_mutex_, bgra, width, height);
#else
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    if (presentation_backend_ &&
        presentation_backend_->capture_front_buffer(bgra, width, height)) {
        return true;
    }
    bgra.clear();
    width = 0;
    height = 0;
    return false;
#endif
}

void Renderer::set_decode_paused_for_all_tracks(bool paused) {
    const TrackDecodePauseHooks hooks{
        [](size_t, TrackPipeline& track, bool paused) {
            track.decode_thread->set_decode_paused(paused);
        },
        [this](bool paused) {
            if (audio_coordinator_) {
                audio_coordinator_->set_all_decode_paused(paused);
            }
        },
    };
    apply_track_decode_pause_state(tracks_, paused, hooks);
}

void Renderer::apply_playback_decode_state_locked(bool playback_active) {
    const TrackPlaybackDecodeStateHooks hooks{
        [](size_t, TrackPipeline& track, bool enabled) {
            track.decode_thread->set_pause_after_preroll(enabled);
        },
        [](size_t, TrackPipeline& track, bool paused) {
            track.decode_thread->set_decode_paused(paused);
        },
        [this](bool paused) {
            if (audio_coordinator_) {
                audio_coordinator_->set_all_decode_paused(paused);
            }
        },
    };
    apply_track_playback_decode_state(tracks_, playback_active, hooks);
}

void Renderer::configure_track_seek_callback(TrackPipeline& track) {
    auto* dt = track.decode_thread.get();
    const int file_id = track.file_id;
    track.demux_thread->set_seek_callback(
        [this, dt, file_id](int64_t pts, SeekType type) {
            if (shutting_down_.load(std::memory_order_acquire)) {
                return;
            }
            dt->notify_seek(pts, type);
            if (audio_coordinator_) {
                audio_coordinator_->notify_seek(file_id, pts, type);
            }
        });
}

void Renderer::configure_track_error_callback(TrackPipeline& track) {
    const int file_id = track.file_id;
    const auto buffer = track.track_buffer;
    track.demux_thread->set_error_callback(
        [this, file_id, buffer](int error_code) {
            if (shutting_down_.load(std::memory_order_acquire)) {
                return;
            }
            if (buffer) {
                buffer->set_state(TrackState::Error);
            }
            RendererEvent event;
            event.type = RendererEvent::Type::TrackError;
            event.track_file_id = file_id;
            event.error_code = error_code;
            emit_event(event);
        });
}

void Renderer::register_track_audio(TrackPipeline& track) {
    if (!audio_coordinator_ ||
        !track.audio_packet_queue ||
        !track.demux_thread ||
        track.file_id < 0) {
        return;
    }
    const auto& stats = track.demux_thread->stats();
    if (!audio_coordinator_->register_track(track.file_id, *track.audio_packet_queue, stats)) {
        spdlog::warn("[Renderer] Failed to start audio decoder for file_id={}", track.file_id);
    }
}

void Renderer::unregister_track_audio(int file_id) {
    if (audio_coordinator_) {
        audio_coordinator_->unregister_track(file_id);
    }
}

int64_t Renderer::effective_duration_us_locked() const {
    return resolve_effective_duration_us(tracks_, cached_duration_us_);
}

bool Renderer::settle_eof_locked(int64_t max_presented_end_us) {
    const int64_t duration_us = effective_duration_us_locked();
    const auto decision = choose_playback_eof_settlement({
        playing_.load(),
        playback_->clock().current_pts_us(),
        max_presented_end_us,
        duration_us,
        compute_min_current_frame_duration_us(tracks_),
    });
    if (!decision.should_settle) {
        return false;
    }

    set_decode_paused_for_all_tracks(true);
    playback_->clock().seek(decision.settle_pts_us);
    playback_->clock().pause();
    playing_ = false;
    preview_drawn_ = true;
    spdlog::info("[Renderer] EOF reached: clock fixed at {:.3f}s (last_frame_end={:.3f}s, duration={:.3f}s)",
                 decision.settle_pts_us / 1e6,
                 max_presented_end_us / 1e6,
                 duration_us / 1e6);
    return true;
}

void Renderer::set_speed(double speed) {
    const auto validation = validate_playback_speed(speed);
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid playback speed: {}", validation.message);
        return;
    }

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    playback_->set_speed(speed);
}

bool Renderer::is_playing() const {
    return playing_;
}

bool Renderer::is_initialized() const {
    return initialized_;
}

int64_t Renderer::current_pts_us() const {
    return playback_->clock().current_pts_us();
}

double Renderer::current_speed() const {
    return playback_->speed();
}

size_t Renderer::track_count() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return tracks_.count();
}

int64_t Renderer::duration_us() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return cached_duration_us_;
}

void Renderer::set_track_offset(int file_id, int64_t offset_us) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    int slot = find_slot_by_file_id(file_id);
    if (slot < 0 || !tracks_[slot]) return;
    const TrackOffsetMutationHooks hooks{
        [this](size_t offset_slot, int64_t offset) {
            render_sink_->set_track_offset(offset_slot, offset);
        },
    };
    apply_track_offset_mutation(
        *tracks_[slot], static_cast<size_t>(slot), offset_us, hooks);
    preview_drawn_ = false;
}

int64_t Renderer::track_offset_us(int file_id) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    int slot = find_slot_by_file_id(file_id);
    if (slot < 0 || !tracks_[slot]) {
        return 0;
    }
    return tracks_[slot]->offset_us;
}

void Renderer::set_frame_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
#ifdef _WIN32
    if (auto* output = headless_output()) {
        output->set_frame_callback(std::move(cb));
        frame_callback_ = {};
        return;
    }
#endif
    frame_callback_ = std::move(cb);
}

void Renderer::set_frame_failure_callback(std::function<void(const char*)> cb) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    frame_failure_callback_ = std::move(cb);
}

void Renderer::set_event_callback(RendererEventCallback cb) {
    std::lock_guard<std::mutex> lock(event_callback_mutex_);
    event_callback_ = std::move(cb);
}

bool Renderer::acquire_shared_texture(SharedTextureSnapshot& snapshot) const {
    snapshot = {};

#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    auto* output = headless_output();
    if (!output) {
        d3d_metrics_.texture_sharing_failure_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::lock_guard<std::mutex> lock(texture_mutex());
    D3D11HeadlessOutputTextureLease lease;
    if (!output->acquire_shared_texture_locked(lease)) {
        d3d_metrics_.texture_sharing_failure_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    snapshot.type = SharedTextureHandleType::D3D11SharedHandle;
    snapshot.texture = lease.texture;
    snapshot.handle = lease.handle;
    snapshot.width = lease.width;
    snapshot.height = lease.height;
    snapshot.buffer_index = lease.buffer_index;
    snapshot.buffer_generation = lease.generation;
    return true;
#else
    return false;
#endif
}

void Renderer::release_shared_texture(int buffer_index, uint64_t buffer_generation) const {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (auto* output = headless_output()) {
        output->release_shared_texture(buffer_index, buffer_generation);
    }
#else
    (void)buffer_index;
    (void)buffer_generation;
#endif
}

std::mutex& Renderer::texture_mutex() const {
#ifdef _WIN32
    auto* output = headless_output();
    return output ? output->texture_mutex() : texture_mutex_fallback_;
#else
    return texture_mutex_fallback_;
#endif
}

void Renderer::resize(int width, int height) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
#ifdef _WIN32
    if (!headless_ || !d3d_device()) return;
#else
    if (!headless_ || !presentation_backend_) return;
#endif
    const auto validation = validate_renderer_dimensions(width, height, "resize dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid resize: {}", validation.message);
        return;
    }
    pending_width_.store(width);
    pending_height_.store(height);
}

bool Renderer::update_headless_output(void* output,
                                      int width,
                                      int height,
                                      int max_track_slots) {
    std::function<void()> frame_callback;
    auto frame_failure_callback = frame_failure_callback_snapshot();
    std::string frame_failure_error;
    bool drew = false;
    bool attempted_draw = false;
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!headless_ || !presentation_backend_) {
            return false;
        }
        const auto validation =
            validate_renderer_dimensions(width, height, "headless output dimensions");
        if (!validation.ok) {
            spdlog::warn("[Renderer] ignoring invalid headless output: {}",
                         validation.message);
            return false;
        }
        {
            std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
            if (!presentation_backend_->update_headless_output(
                    output, width, height, max_track_slots)) {
                return false;
            }
        }

        RendererDrawSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto old_width = target_width_;
            const auto old_height = target_height_;
            if (old_width != width || old_height != height) {
                const auto layout_tracks = snapshot_layout_track_geometry(tracks_);
                adjust_layout_view_offset_for_resize(
                    layout_, old_width, old_height, width, height, layout_tracks);
                target_width_ = width;
                target_height_ = height;
            }
            snapshot = build_draw_snapshot_locked(last_decision_);
        }
        if (present_decision_has_frame(snapshot.decision)) {
            attempted_draw = true;
            std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
            drew = draw_frame(snapshot);
            if (drew) {
                frame_callback = frame_callback_;
            } else {
                frame_failure_error = presentation_backend_last_error();
            }
        }
    }
    if (drew) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        preview_drawn_ = true;
    }
    if (frame_callback && !shutting_down_.load(std::memory_order_acquire)) {
        frame_callback();
    }
    if (attempted_draw && !drew && frame_failure_callback &&
        !shutting_down_.load(std::memory_order_acquire)) {
        frame_failure_callback(frame_failure_error.c_str());
    }
    return true;
}

void Renderer::clear_headless_output() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_ || !presentation_backend_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    presentation_backend_->clear_headless_output();
}

void Renderer::do_resize(int width, int height) {
#ifdef _WIN32
    int old_width = 0;
    int old_height = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (width == target_width_ && height == target_height_) return;
        old_width = target_width_;
        old_height = target_height_;
    }

    spdlog::info("[Renderer] resize: {}x{} -> {}x{}", old_width, old_height, width, height);

    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        std::lock_guard<std::mutex> tex_lock(texture_mutex());
        auto* output = headless_output();
        if (!output || !output->resize_locked(width, height)) {
            return;
        }
    }
    d3d_metrics_.shared_texture_resize_count.fetch_add(1, std::memory_order_relaxed);

    RendererDrawSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto layout_tracks = snapshot_layout_track_geometry(tracks_);
        adjust_layout_view_offset_for_resize(
            layout_, old_width, old_height, width, height, layout_tracks);
        target_width_ = width;
        target_height_ = height;
        snapshot = build_draw_snapshot_locked(last_decision_);
    }

    std::function<void()> frame_callback;
    bool drew = false;
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        drew = draw_headless_and_publish(snapshot, "resize", frame_callback);
    }
    if (drew) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        preview_drawn_ = true;
    }
    if (frame_callback && !shutting_down_.load(std::memory_order_acquire)) {
        frame_callback();
    }
#else
    (void)width;
    (void)height;
#endif
}

void Renderer::render_loop() noexcept {
    // Raise Windows timer resolution from default ~15.6ms to 1ms,
    // so sleep_for(16ms) actually wakes up near 16ms instead of 31ms.
    ScopedRenderThreadTiming render_thread_timing;
    spdlog::info("[Renderer] Render loop started (timer resolution: platform), tid={}",
                 current_render_thread_id_string());

    try {
        render_loop_body();
    } catch (const std::exception& e) {
        spdlog::error("[Renderer] Render loop crashed: {}", e.what());
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_render_loop_error_locked("std::exception");
    } catch (...) {
        spdlog::error("[Renderer] Render loop crashed with an unknown exception");
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_render_loop_error_locked("unknown exception");
    }

    pending_width_.store(0, std::memory_order_release);
    pending_height_.store(0, std::memory_order_release);
    spdlog::info("[Renderer] Render loop ended");
}

void Renderer::render_loop_body() {
    render_loop_controller_.start(std::chrono::steady_clock::now());

    while (running_) {
        auto* backend = presentation_backend_.get();
        if (backend && backend->poll_device_removed("render_loop")) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            enter_terminal_device_lost_locked("render_loop");
            break;
        }

        {
            std::unique_lock<std::mutex> lifecycle_lock(
                lifecycle_mutex_, std::try_to_lock);
            if (lifecycle_lock.owns_lock()) {
                std::unique_lock<std::mutex> lock(state_mutex_);
                if (apply_deferred_paused_hevc_seek_locked(lock)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (apply_loop_range_locked(lock)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }
        }

        // Process pending resize (debounced — at most ~30Hz).
        {
            int pw = pending_width_.exchange(0);
            int ph = pending_height_.exchange(0);
            if (pw > 0 && ph > 0) {
                auto now = std::chrono::steady_clock::now();
                if (render_loop_controller_.should_apply_resize(now)) {
                    do_resize(pw, ph);
                    render_loop_controller_.mark_resize_applied(now);
                } else {
                    // Too soon — re-queue so the next iteration can pick it up.
                    // Write back only if no newer resize arrived in the meantime.
                    int expected = 0;
                    pending_width_.compare_exchange_strong(expected, pw);
                    expected = 0;
                    pending_height_.compare_exchange_strong(expected, ph);
                }
            }
        }

#ifdef _WIN32
        if (auto* output = headless_output()) {
            output->cleanup_expired_pending_buffers();
        }
#endif

        bool playing_snapshot;
        bool clock_paused_snapshot;
        bool log_preroll_transition = false;
        bool log_preroll_pending = false;
        bool log_preroll_complete = false;
        double preroll_complete_pts_s = -1.0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            playing_snapshot = playing_;

            // Preroll: keep clock paused while any track is still buffering.
            const bool any_buffering = has_preroll_blocking_track(tracks_);

            // Detect Buffering -> Ready transition: force preview redraw so the
            // newly-ready track's first frame appears on screen immediately
            // (even while paused -- matches initialize() behavior).
            if (was_buffering_ && !any_buffering) {
                preview_drawn_ = false;
                last_decision_ = PresentDecision();  // Clear stale cached frames
                log_preroll_transition = true;
            }
            was_buffering_ = any_buffering;

            clock_paused_snapshot = playback_->clock().is_paused();
            if (any_buffering && !clock_paused_snapshot) {
                playback_->clock().pause();
                clock_paused_snapshot = true;
                log_preroll_pending = true;
            } else if (!any_buffering && clock_paused_snapshot && playing_snapshot) {
                set_decode_paused_for_all_tracks(false);
                playback_->clock().resume();
                preview_drawn_ = false;
                clock_paused_snapshot = false;
                preroll_complete_pts_s =
                    static_cast<double>(playback_->clock().current_pts_us()) / 1e6;
                log_preroll_complete = true;
            }
        }
        if (log_preroll_transition) {
            spdlog::info("[Renderer] Preroll transition complete, forcing preview redraw");
        }
        if (log_preroll_pending) {
            spdlog::info("[Renderer] Preroll: clock PENDING, some track buffering, "
                         "(playing={})", playing_snapshot);
        }
        if (log_preroll_complete) {
            spdlog::info("[Renderer] === Preroll COMPLETE: all tracks ready, clock resumed, "
                         "playing_={}, pts={:.3f}s)",
                         playing_snapshot, preroll_complete_pts_s);
        }

        if (!playing_snapshot || clock_paused_snapshot) {
            // While paused/prerolling, draw current frame if not yet drawn
            bool should_draw_preview = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                should_draw_preview = !preview_drawn_;
            }
            if (should_draw_preview) {
                bool drawn = false;

                // Try cached last frame first (for layout changes while paused)
                PresentDecision cached_decision;
                std::optional<int64_t> cached_pts_us;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    filter_present_decision_against_tracks(last_decision_, tracks_);
                    if (present_decision_has_frame(last_decision_)) {
                        cached_decision = last_decision_;
                        cached_pts_us =
                            first_present_decision_frame_pts_us(cached_decision);
                    }
                }
                if (present_decision_has_frame(cached_decision)) {
                    present_frame(cached_decision);
                    drawn = true;
                    spdlog::debug("[Renderer] Paused frame (cached): pts={:.3f}s",
                                  cached_pts_us.has_value()
                                      ? static_cast<double>(*cached_pts_us) / 1e6
                                      : -1.0);
                }

                // No cached frame — try track buffer (initial preview)
                // Only draw when ALL active tracks have frames, to avoid
                // flashing black for tracks that haven't finished seeking.
                if (!drawn) {
                    PausedPreviewSnapshot snapshot;
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        snapshot = build_paused_preview_snapshot(tracks_);
                    }
                    if (snapshot.ready_to_present) {
                        auto& preview = snapshot.decision;
                        present_frame(preview);
                        bool preserve_requested_clock = false;
                        int ref = -1;
                        int64_t ref_offset_us = 0;
                        if (!playing_snapshot) {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            last_decision_ = preview;
                            set_decode_paused_for_all_tracks(true);
                            preserve_requested_clock = true;
                            mark_paused_hevc_seek_preview_drawn_locked();
                            ref = first_active_track();
                            if (ref >= 0 && tracks_[ref]) {
                                ref_offset_us = tracks_[ref]->offset_us;
                            }
                        } else {
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            last_decision_ = preview;
                            ref = first_active_track();
                            if (ref >= 0 && tracks_[ref]) {
                                ref_offset_us = tracks_[ref]->offset_us;
                            }
                        }
                        // Keep the logical clock at the user's requested target
                        // while paused. The decoded preview can land on a
                        // nearest/tail frame for individual tracks, but the
                        // timeline should not visually snap backward.
                        if (!preserve_requested_clock &&
                            ref >= 0 &&
                            preview.frames[ref].has_value()) {
                            playback_->clock().seek(
                                preview.frames[ref]->pts_us + ref_offset_us);
                        }
                        spdlog::info("[Renderer] Paused frame: pts={:.3f}s",
                                     ref >= 0 && preview.frames[ref].has_value()
                                     ? preview.frames[ref]->pts_us / 1e6 : -1.0);
                        emit_seek_preview_presented_events(preview);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto decision = render_sink_->evaluate();
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            filter_present_decision_against_tracks(decision, tracks_);
        }

        // Periodic diagnostics
        {
            auto now = std::chrono::steady_clock::now();
            int64_t pts = playback_->clock().current_pts_us();
            int64_t pts_delta = 0;
            if (render_loop_controller_.should_emit_diagnostics(now, pts, pts_delta)) {
                std::vector<RenderLoopTrackDiagnosticSnapshot> diagnostics;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    diagnostics = snapshot_render_loop_track_diagnostics(tracks_);
                }
                for (const auto& track : diagnostics) {
                    spdlog::info("[diag] track[{}]: pts={:.3f}s delta={:.1f}ms "
                                 "buf={}/{} state={} playing={}",
                                 track.slot, pts / 1e6, pts_delta / 1e3,
                                 track.buffer_count, track.buffer_capacity,
                                 static_cast<int>(track.buffer_state),
                                 playing_snapshot);
                }
            }
        }

        if (decision.should_present) {
            // Independent presentation: fill missing tracks from last decision
            // so each track always shows a frame (new or carried over).
            // Once a track has started, keep carrying its last frame even after
            // that track reaches EOF. This lets shorter tracks freeze on their
            // final image while longer tracks continue playing.
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                apply_present_carry_forward(tracks_, last_decision_, decision);
            }
            present_frame(decision);
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                last_decision_ = decision;
            }
        } else {
            // No new frame but layout changed (e.g. zoom/pan during playback)
            bool should_redraw = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                should_redraw =
                    !preview_drawn_ && present_decision_has_frame(last_decision_);
            }
            if (should_redraw) {
                redraw_layout();
            }
        }

        // Frame-driven clock: when buffer is empty, clamp clock to the
        // end of the last presented frame so PTS doesn't run ahead.
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto eof_clamp =
                compute_empty_buffer_eof_clamp(tracks_, last_decision_);
            if (eof_clamp.all_active_buffers_empty &&
                eof_clamp.max_end_pts_us > 0) {
                int64_t current = playback_->clock().current_pts_us();
                if (current > eof_clamp.max_end_pts_us) {
                    playback_->clock().seek(eof_clamp.max_end_pts_us);
                }
                if (settle_eof_locked(eof_clamp.max_end_pts_us)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }
        }

        // Deadline-based sleep: wake up at the exact wall time when the next
        // frame should be displayed.  This is drift-free because each sleep
        // targets an absolute PTS rather than an accumulated relative duration.
        {
            int64_t current_pts = playback_->clock().current_pts_us();
            std::optional<int64_t> next_event_pts;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                next_event_pts =
                    compute_next_frame_event_pts_us(tracks_, current_pts);
            }
            if (next_event_pts.has_value()) {
                double spd = playback_->clock().speed();
                const auto sleep_for = render_loop_controller_.frame_deadline_sleep(
                    current_pts, *next_event_pts, spd, MAX_SLEEP_US);
                if (sleep_for.count() > 0) {
                    std::this_thread::sleep_for(sleep_for);
                }
            } else {
                // No frames available (buffer underflow) — short poll
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    pending_width_.store(0, std::memory_order_release);
    pending_height_.store(0, std::memory_order_release);
}

bool Renderer::draw_frame(const RendererDrawSnapshot& snapshot) {
    auto* backend = presentation_backend_.get();
    if (!backend) {
        return false;
    }
    PresentationBackendDrawHooks hooks;
    hooks.wait_gpu_idle = [this](const char* label) { wait_gpu_idle(label); };
    hooks.record_frame_copy_us = [this](uint64_t elapsed_us) {
        d3d_metrics_.frame_copy_us.fetch_add(elapsed_us, std::memory_order_relaxed);
        d3d_metrics_.frame_copy_count.fetch_add(1, std::memory_order_relaxed);
    };
    hooks.draw_overlay = [this](PresentationBackend& backend,
                                const RendererDrawSnapshot& draw_snapshot) {
#ifdef _WIN32
        if (!analysis_overlay_renderer_ ||
            backend.kind() != PresentationBackendKind::D3D11) {
            return;
        }
        auto* d3d = static_cast<D3D11RenderBackend*>(&backend);
        auto* device = d3d->device();
        auto* resources = d3d->resources();
        if (!device || !resources) {
            return;
        }
        analysis_overlay_renderer_->draw(
            draw_snapshot.decision,
            draw_snapshot.tracks,
            *device,
            *resources,
            draw_snapshot.target_width,
            draw_snapshot.target_height);
#else
        (void)backend;
        (void)draw_snapshot;
#endif
    };
    return backend->draw_frame(snapshot, hooks);
}

// -- Layout control --
void Renderer::apply_layout(const LayoutState& state) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (auto validation = validate_layout_state(state); !validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid layout: {}", validation.message);
        return;
    }
    layout_controller_.apply(
        layout_, state, [this](int file_id) { return find_slot_by_file_id(file_id); });

    // Trigger redraw — during playback, redraw_layout() handles this
    // without Flush() to avoid contention with D3D11VA decode threads.
    preview_drawn_ = false;
}

void Renderer::set_background_color(float r, float g, float b, float a) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    background_color_[0] = std::clamp(r, 0.0f, 1.0f);
    background_color_[1] = std::clamp(g, 0.0f, 1.0f);
    background_color_[2] = std::clamp(b, 0.0f, 1.0f);
    background_color_[3] = std::clamp(a, 0.0f, 1.0f);
    preview_drawn_ = false;
}

LayoutState Renderer::layout() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return layout_controller_.snapshot(layout_);
}

// -- Dynamic track management --

int Renderer::first_active_track() const {
    return tracks_.first_active_slot();
}

int Renderer::find_slot_by_file_id(int file_id) const {
    return tracks_.find_slot_by_file_id(file_id);
}

int Renderer::find_empty_slot() const {
    return tracks_.find_empty_slot();
}

std::unique_ptr<TrackPipeline> Renderer::create_pipeline(
    const std::string& path,
    bool hw_decode,
    const SeekRequest* initial_seek) {
    TrackPipelineOpenOptions options;
    options.render_backend = render_backend_kind_;
    return track_pipeline_factory_.create_opened_pipeline(
        path, hw_decode, initial_seek, options);
}

bool Renderer::recreate_pipeline_for_seek(std::unique_lock<std::mutex>& state_lock,
                                          size_t slot,
                                          int64_t target_pts_us,
                                          SeekType type) {
    if (!state_lock.owns_lock()) {
        spdlog::error("[Renderer] recreate_pipeline_for_seek called without state lock");
        return false;
    }
    if (slot >= kMaxTracks || !tracks_[slot]) {
        return false;
    }

    auto current = std::move(tracks_[slot]);
    const auto file_path = current->file_path;
    const auto file_id = current->file_id;
    const auto offset_us = current->offset_us;
    const auto use_hardware_decode = current->use_hardware_decode;
    const auto generation = next_track_generation_++;

    spdlog::info("[Renderer] Recreating pipeline for {}", file_path);

    unregister_track_audio(file_id);
    render_sink_->set_track(slot, nullptr);
    if (presentation_backend_) {
        presentation_backend_->reset_track(slot);
    }
    clear_present_decision_slot(last_decision_, slot);
    preview_drawn_ = false;

    state_lock.unlock();

    stop_detached_track_pipeline(slot, current);

    // Give the driver a brief moment to retire the previous D3D11VA decoder
    // objects before constructing a fresh hardware pipeline on the same file.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const SeekRequest initial_seek{target_pts_us, type};
    auto replacement = create_pipeline(file_path, use_hardware_decode, &initial_seek);
    if (!replacement) {
        spdlog::error("[Renderer] Failed to recreate pipeline for {}", file_path);
        state_lock.lock();
        return false;
    }
    replacement->generation = generation;

    const TrackPipelineStartConfig start_config{
        file_id,
        offset_us,
        false,
        true,
    };
    const TrackPipelineStartHooks start_hooks{
        [this](TrackPipeline& track) { configure_track_seek_callback(track); },
        [this](TrackPipeline& track) { configure_track_error_callback(track); },
        [this](TrackPipeline& track) { register_track_audio(track); },
        [this](int id) { unregister_track_audio(id); },
    };
    if (!configure_and_start_track_pipeline(
            *replacement, start_config, start_hooks, "[Renderer]")) {
        state_lock.lock();
        return false;
    }

    state_lock.lock();
    if (!initialized_ || tracks_[slot]) {
        state_lock.unlock();
        stop_detached_track_pipeline(slot, replacement);
        state_lock.lock();
        return false;
    }

    render_sink_->set_track(slot, replacement->track_buffer, file_id, generation);
    render_sink_->set_track_offset(slot, offset_us);
    tracks_[slot] = std::move(replacement);
    return true;
}

int Renderer::add_track(const std::string& video_path,
                        bool use_hardware_decode) {
    return add_track_internal(video_path, use_hardware_decode, -1);
}

int Renderer::add_track_with_file_id(const std::string& video_path,
                                     int file_id,
                                     bool use_hardware_decode) {
    if (file_id < 0) {
        spdlog::warn("Renderer::add_track_with_file_id: invalid file_id={}", file_id);
        return -1;
    }
    return add_track_internal(video_path, use_hardware_decode, file_id);
}

int Renderer::add_track_internal(const std::string& video_path,
                                 bool use_hardware_decode,
                                 int requested_file_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    int slot = -1;
    int new_file_id = 0;
    uint64_t new_generation = 0;
    int64_t current_pts = 0;
    TrackPlaybackMutationState playback_state;

    const TrackPlaybackMutationHooks playback_hooks{
        [this]() { playback_->pause(); },
        [this]() { playback_->play(); },
        [this](bool playing) { playing_ = playing; },
    };

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_) return -1;

        slot = find_empty_slot();
        if (slot < 0) {
            spdlog::warn("Renderer::add_track: no empty slots");
            return -1;
        }
        if (requested_file_id >= 0 && find_slot_by_file_id(requested_file_id) >= 0) {
            spdlog::warn("Renderer::add_track: file_id={} is already open",
                         requested_file_id);
            return -1;
        }

        playback_state = pause_playback_for_track_mutation(
            playing_.load(), playback_hooks);
        if (requested_file_id >= 0) {
            new_file_id = requested_file_id;
            next_file_id_ = std::max(next_file_id_, requested_file_id + 1);
        } else {
            new_file_id = next_file_id_++;
        }
        new_generation = next_track_generation_++;
        current_pts = playback_->clock().current_pts_us();
    }

    auto pipeline = create_pipeline(video_path, use_hardware_decode);
    if (!pipeline) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        rollback_track_mutation_playback(playback_state, playback_hooks);
        return -1;
    }
    pipeline->generation = new_generation;
    const TrackPipelineStartConfig start_config{
        new_file_id,
        0,
        !playback_state.was_playing,
        false,
    };
    const TrackPipelineStartHooks hooks{
        [this](TrackPipeline& track) { configure_track_seek_callback(track); },
        [this](TrackPipeline& track) { configure_track_error_callback(track); },
        [this](TrackPipeline& track) { register_track_audio(track); },
        [this](int id) { unregister_track_audio(id); },
    };
    if (!configure_and_start_track_pipeline(
            *pipeline, start_config, hooks, "Renderer::add_track")) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        rollback_track_mutation_playback(playback_state, playback_hooks);
        return -1;
    }

    TrackAddSeekResult seek_result;
    int64_t track_offset_us = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_ ||
            slot < 0 ||
            slot >= static_cast<int>(kMaxTracks) ||
            tracks_[static_cast<size_t>(slot)]) {
            rollback_track_mutation_playback(playback_state, playback_hooks);
            return -1;
        }

        // Update duration cache
        cached_duration_us_ = extend_track_duration_cache(cached_duration_us_, *pipeline);

        const TrackAddCommitHooks commit_hooks{
            [this](size_t committed_slot, TrackPipeline& track) {
                render_sink_->set_track(
                    committed_slot, track.track_buffer, track.file_id, track.generation);
                render_sink_->set_track_offset(committed_slot, track.offset_us);
            },
            [this](size_t committed_slot) {
                if (presentation_backend_) {
                    presentation_backend_->reset_track(committed_slot);
                }
            },
        };
        TrackPipeline* track = commit_new_track_pipeline(
            tracks_, static_cast<size_t>(slot), std::move(pipeline), commit_hooks);
        if (!track) {
            rollback_track_mutation_playback(playback_state, playback_hooks);
            return -1;
        }

        layout_controller_.append_track(layout_, new_file_id, slot);

        // Seek new track to current clock position so evaluate() can find matching frames.
        // Without this, the new track starts from PTS=0 and evaluate() discards all its
        // frames as "expired" when the clock is elsewhere, causing both panels to show
        // the same old video.
        const TrackAddSeekHooks add_seek_hooks{
            [this](int file_id, bool paused) {
                if (audio_coordinator_) {
                    audio_coordinator_->set_track_decode_paused(file_id, paused);
                }
            },
        };
        seek_result = prepare_add_track_seek_to_clock(
            *track, current_pts, playback_state.was_playing, add_seek_hooks);
        track_offset_us = track->offset_us;

        // Force redraw, but keep already-presented frames from existing tracks so
        // they remain visible while the new track is still buffering/soft-decoding.
        preview_drawn_ = false;
        last_decision_.frames[slot] = std::nullopt;
        last_decision_.file_ids[slot] = -1;
        last_decision_.track_generations[slot] = 0;
    }

    if (seek_result.applied) {
        spdlog::info("Renderer::add_track: seeking slot={} to {:.3f}s (offset={:.3f}s, type={})",
                     slot,
                     seek_result.target_pts_us / 1e6,
                     track_offset_us / 1e6,
                     is_exact_seek_type(seek_result.seek_type) ? "Exact" : "Keyframe");
    }

    spdlog::info("Renderer::add_track: slot={} hw_decode={} path={}",
                 slot,
                 use_hardware_decode,
                 video_path);
    return slot;
}

void Renderer::remove_track(int file_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::unique_ptr<TrackPipeline> removed_track;
    int slot = -1;
    size_t remaining = 0;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        slot = find_slot_by_file_id(file_id);
        if (slot < 0) return;

        spdlog::info("Renderer::remove_track: file_id={}, slot={}", file_id, slot);

        const TrackPlaybackMutationHooks playback_hooks{
            [this]() { playback_->pause(); },
            [this]() { playback_->play(); },
            [this](bool playing) { playing_ = playing; },
        };
        const auto playback_state = pause_playback_for_track_mutation(
            playing_.load(), playback_hooks);

        unregister_track_audio(file_id);
        render_sink_->set_track(static_cast<size_t>(slot), nullptr);
        if (presentation_backend_) {
            presentation_backend_->reset_track(static_cast<size_t>(slot));
        }
        removed_track = std::move(tracks_[static_cast<size_t>(slot)]);
        if (removed_track && removed_track->demux_thread) {
            removed_track->demux_thread->set_seek_callback({});
            removed_track->demux_thread->set_error_callback({});
        }

        tracks_.compact_from(static_cast<size_t>(slot), [this](
            size_t from,
            size_t to,
            TrackPipeline& track) {
            if (presentation_backend_) {
                presentation_backend_->move_track(from, to);
            }
            render_sink_->set_track(to, track.track_buffer, track.file_id, track.generation);
            render_sink_->set_track_offset(to, track.offset_us);
            render_sink_->set_track(from, nullptr);
        });
        compact_present_decision_frames(last_decision_, static_cast<size_t>(slot));

        layout_controller_.remove_track(
            layout_, file_id, [this](int id) { return find_slot_by_file_id(id); });

        cached_duration_us_ = compute_track_duration_cache(tracks_);
        preview_drawn_ = false;
        remaining = tracks_.count();

        finish_track_removal_playback(
            playback_state, first_active_track() >= 0, playback_hooks);
    }

    if (removed_track) {
        if (removed_track->decode_thread) {
            spdlog::info("Renderer: stopping track[{}] decode ({})",
                         slot,
                         removed_track->file_path);
            removed_track->decode_thread->stop();
            spdlog::info("Renderer: track[{}] decode stopped", slot);
        }
        if (removed_track->demux_thread) {
            spdlog::info("Renderer: stopping track[{}] demux ({})",
                         slot,
                         removed_track->file_path);
            removed_track->demux_thread->stop();
            spdlog::info("Renderer: track[{}] demux stopped", slot);
        }
        removed_track.reset();
    }

    spdlog::info(
        "Renderer::remove_track: file_id={}, slot={}, remaining={}",
        file_id,
        slot,
        remaining);
}

bool Renderer::has_track(int slot) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks)) return false;
    return tracks_[slot] != nullptr;
}

std::pair<int, int> Renderer::track_dimensions(int slot) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks) || !tracks_[slot]) {
        return {0, 0};
    }
    return {tracks_[slot]->video_width, tracks_[slot]->video_height};
}

std::vector<TrackInfo> Renderer::track_infos() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return snapshot_track_infos(tracks_);
}

std::vector<TrackPerfStats> Renderer::track_perf_stats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto now = std::chrono::steady_clock::now();
    const double elapsed_s = perf_baseline_tracker_.elapsed_seconds(now);
    const bool should_rotate = perf_baseline_tracker_.should_rotate(elapsed_s);
    PresentDecision filtered_last_decision = last_decision_;
    filter_present_decision_against_tracks(filtered_last_decision, tracks_);
    const auto snapshot = snapshot_track_perf_stats_collection(
        tracks_, filtered_last_decision, perf_baseline_tracker_, elapsed_s);

    if (should_rotate) {
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!snapshot.frames_decoded_by_slot[i].has_value()) {
                continue;
            }
            perf_baseline_tracker_.update_baseline_frames(
                i, *snapshot.frames_decoded_by_slot[i]);
        }
    }

    // Reset shared timer once after all tracks are processed
    if (should_rotate) {
        perf_baseline_tracker_.rotate_timer(now);
    }
    return snapshot.stats;
}

PresentationBackendMetrics Renderer::presentation_backend_metrics() const {
    PresentationBackendMetrics result;
    result.render_wait_us = d3d_metrics_.render_wait_us.load(std::memory_order_relaxed);
    result.render_wait_count = d3d_metrics_.render_wait_count.load(std::memory_order_relaxed);
    result.frame_copy_us = d3d_metrics_.frame_copy_us.load(std::memory_order_relaxed);
    result.frame_copy_count = d3d_metrics_.frame_copy_count.load(std::memory_order_relaxed);
    result.present_publish_us = d3d_metrics_.present_publish_us.load(std::memory_order_relaxed);
    result.present_publish_count =
        d3d_metrics_.present_publish_count.load(std::memory_order_relaxed);
    result.shared_texture_resize_count =
        d3d_metrics_.shared_texture_resize_count.load(std::memory_order_relaxed);
    result.device_lost_count = d3d_metrics_.device_lost_count.load(std::memory_order_relaxed);
    result.texture_sharing_failure_count =
        d3d_metrics_.texture_sharing_failure_count.load(std::memory_order_relaxed);
    return result;
}

D3D11BackendMetrics Renderer::d3d_backend_metrics() const {
    return presentation_backend_metrics();
}

PresentationBackendStats Renderer::presentation_backend_stats() const {
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    if (!presentation_backend_) {
        return {};
    }
    return presentation_backend_->presentation_stats();
}

bool Renderer::copy_last_presentation_frame_info(
    PresentationBackendFrameInfo* out) const {
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    return presentation_backend_ && presentation_backend_->copy_last_frame_info(out);
}

RendererGpuMemoryStats Renderer::gpu_memory_stats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    std::lock_guard<std::recursive_mutex> device_lock(device_mutex_);
    RendererGpuMemoryStats result;

#ifdef _WIN32
    auto* presenter = frame_presenter();
    const auto presenter_stats = presenter
        ? presenter->memory_stats()
        : D3D11FramePresenterMemoryStats{};
    result.presenter_texture_bytes = presenter_stats.total_estimated_bytes;
    result.total_estimated_bytes += result.presenter_texture_bytes;

    if (auto* output = headless_output()) {
        const auto headless_stats = output->memory_stats();
        result.headless_output_bytes = headless_stats.estimated_bytes;
        result.headless_width = headless_stats.width;
        result.headless_height = headless_stats.height;
        result.headless_buffer_count = headless_stats.buffer_count;
        result.total_estimated_bytes += result.headless_output_bytes;
    }

    if (auto* resources = d3d_resources()) {
        const auto overlay_stats =
            snapshot_analysis_overlay_memory_stats(*resources);
        result.analysis_overlay_bytes = overlay_stats.estimated_bytes;
        result.analysis_overlay_width = overlay_stats.width;
        result.analysis_overlay_height = overlay_stats.height;
        if (result.analysis_overlay_bytes > 0) {
            result.total_estimated_bytes += result.analysis_overlay_bytes;
        }
    }

    std::array<uint64_t, kMaxTracks> presenter_copy_texture_bytes_by_slot{};
    for (size_t i = 0; i < kMaxTracks && i < presenter_stats.slots.size(); ++i) {
        presenter_copy_texture_bytes_by_slot[i] =
            presenter_stats.slots[i].render_nv12_copy_texture_bytes;
    }
#else
    std::array<uint64_t, kMaxTracks> presenter_copy_texture_bytes_by_slot{};
#endif
    const auto track_memory = snapshot_track_gpu_memory_stats_collection(
        tracks_, presenter_copy_texture_bytes_by_slot);
    result.decoder_pool_bytes += track_memory.decoder_pool_bytes;
    result.exact_seek_snapshot_bytes += track_memory.exact_seek_snapshot_bytes;
    result.track_buffer_cpu_bytes += track_memory.track_buffer_cpu_bytes;
    result.packet_queue_bytes += track_memory.packet_queue_bytes;
    result.exact_seek_candidate_cpu_bytes +=
        track_memory.exact_seek_candidate_cpu_bytes;
    result.exact_seek_stable_cpu_bytes += track_memory.exact_seek_stable_cpu_bytes;
    result.exact_seek_budget_drop_count += track_memory.exact_seek_budget_drop_count;
    result.cpu_frame_bytes += track_memory.cpu_frame_bytes;
    result.total_estimated_bytes += track_memory.total_estimated_bytes;
    result.tracks = track_memory.tracks;

    return result;
}

bool Renderer::d3d_device_lost() const {
    return device_state_.load(std::memory_order_acquire) != RendererDeviceState::Ready ||
           (presentation_backend_ && presentation_backend_->device_lost());
}

long Renderer::d3d_device_removed_reason() const {
    return presentation_backend_ ? presentation_backend_->device_removed_reason() : 0;
}

RendererDeviceState Renderer::device_state() const {
    return device_state_.load(std::memory_order_acquire);
}

} // namespace vr
