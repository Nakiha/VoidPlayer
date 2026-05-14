#include "video_renderer/renderer.h"
#include "video_renderer/analysis_overlay_renderer.h"
#include "video_renderer/layout_validation.h"
#include "video_renderer/renderer_config_validation.h"
#include "audio/audio_output_factory.h"
#include "video_renderer/audio_coordinator.h"
#include "video_renderer/seek_coordinator.h"
#include "video_renderer/shader_constants.h"
#include "video_renderer/d3d11/render_backend.h"
#include "video_renderer/d3d11/memory_estimate.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

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

int64_t track_pts_end_us_from_stats(const DemuxStats& stats) {
    if (stats.duration_us <= 0) {
        return 0;
    }
    if (stats.start_time_us <= 0) {
        return stats.duration_us;
    }
    // Most containers expose duration as a span. Some FLV files expose a value
    // closer to the absolute end PTS; detect those so a -start offset maps the
    // track to its actual playable span instead of the full PTS epoch.
    if (stats.duration_us > stats.start_time_us &&
        stats.duration_us - stats.start_time_us < stats.duration_us / 2) {
        return stats.duration_us;
    }
    return stats.start_time_us + stats.duration_us;
}

bool is_h264_flv_track(const TrackPipeline& track) {
    if (!track.demux_thread) {
        return false;
    }
    const auto& stats = track.demux_thread->stats();
    if (!stats.codec_params || stats.codec_params->codec_id != AV_CODEC_ID_H264) {
        return false;
    }
    std::string format = stats.format_name;
    std::transform(format.begin(), format.end(), format.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return format.find("flv") != std::string::npos;
}

Renderer::Renderer()
    : owned_playback_(std::make_unique<PlaybackController>(create_default_audio_output))
    , playback_(owned_playback_.get())
    , audio_coordinator_(std::make_unique<AudioCoordinator>(*playback_))
    , seek_coordinator_(std::make_unique<SeekCoordinator>(kPausedHevcSeekSettleDelay))
    , analysis_overlay_renderer_(std::make_unique<AnalysisOverlayRenderer>()) {}

Renderer::Renderer(PlaybackController& playback)
    : playback_(&playback)
    , audio_coordinator_(std::make_unique<AudioCoordinator>(*playback_))
    , seek_coordinator_(std::make_unique<SeekCoordinator>(kPausedHevcSeekSettleDelay))
    , analysis_overlay_renderer_(std::make_unique<AnalysisOverlayRenderer>()) {}

Renderer::~Renderer() {
    shutdown();
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
    device_state_.store(RendererDeviceState::Ready, std::memory_order_release);
    reset_d3d_metrics();
    playback_session_started_by_renderer_ = false;
    if (!playback_->audio_output()) {
        playback_->start_session();
        playback_session_started_by_renderer_ = true;
    }

    d3d_backend_ = std::make_unique<D3D11RenderBackend>();
    D3D11RenderBackendConfig backend_config;
    backend_config.hwnd = hwnd_;
    backend_config.adapter = config.backend.adapter;
    backend_config.width = target_width_;
    backend_config.height = target_height_;
    backend_config.headless = config.headless;
    if (!d3d_backend_->initialize(backend_config)) {
        return fail();
    }
    d3d_device_ = d3d_backend_->device();
    texture_mgr_ = d3d_backend_->texture_manager();
    frame_presenter_ = d3d_backend_->frame_presenter();
    headless_output_ = d3d_backend_->headless_output();
    shader_mgr_ = d3d_backend_->shader_manager();
    d3d_resources_ = d3d_backend_->resources();

    // Create tracks
    for (const auto& path : config.video_paths) {
        int slot = find_empty_slot();
        if (slot < 0) {
            spdlog::warn("Renderer: skipping {}, max {} tracks", path, kMaxTracks);
            continue;
        }

        auto pipeline = create_pipeline(path, config.use_hardware_decode);
        if (!pipeline) continue;
        pipeline->decode_thread->set_pause_after_preroll(true);

        pipeline->file_id = next_file_id_++;
        configure_track_seek_callback(*pipeline);
        configure_track_error_callback(*pipeline);
        register_track_audio(*pipeline);
        if (!pipeline->demux_thread->start_thread()) {
            spdlog::error("Renderer: failed to start demux thread for {}", path);
            unregister_track_audio(pipeline->file_id);
            pipeline->decode_thread->stop();
            pipeline->demux_thread->stop();
            continue;
        }
        tracks_[slot] = std::move(pipeline);
    }

    bool any_track = false;
    for (const auto& t : tracks_) { if (t) { any_track = true; break; } }
    if (!any_track) {
        spdlog::error("Renderer: no valid tracks");
        return fail();
    }

    layout_controller_.reset(layout_);
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            layout_controller_.append_track(
                layout_, tracks_[i]->file_id, static_cast<int>(i));
        }
    }

    // Setup render sink
    render_sink_ = std::make_unique<RenderSink>(playback_->clock());
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            render_sink_->set_track(i, tracks_[i]->track_buffer);
        }
    }

    // Cache duration (immutable after init)
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            cached_duration_us_ = std::max(cached_duration_us_,
                tracks_[i]->demux_thread->stats().duration_us);
        }
    }

    initialized_ = true;

    // Initialize perf stats baseline
    stats_start_time_ = std::chrono::steady_clock::now();
    for (auto& bl : perf_baselines_) bl.frames = 0;

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

    spdlog::info("Renderer: initialized with {} tracks", track_count());
    return true;
}

void Renderer::shutdown() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    bool has_resources = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (const auto& track : tracks_) {
            if (track) {
                has_resources = true;
                break;
            }
        }
        has_resources = has_resources ||
                        d3d_device_ ||
                        d3d_backend_ ||
                        texture_mgr_ ||
                        frame_presenter_ ||
                        headless_output_ ||
                        shader_mgr_ ||
                        render_sink_ ||
                        d3d_resources_ ||
                        initialized_.load() ||
                        running_.load() ||
                        render_thread_.joinable();
        if (!has_resources) {
            return;
        }

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

void Renderer::release_resources_locked() {
    running_ = false;
    playing_ = false;

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
    shader_mgr_ = nullptr;
    frame_presenter_ = nullptr;
    texture_mgr_ = nullptr;
    d3d_resources_ = nullptr;
    headless_output_ = nullptr;
    d3d_device_ = nullptr;
    if (d3d_backend_) {
        d3d_backend_->shutdown();
        d3d_backend_.reset();
    }

    hwnd_ = nullptr;
    headless_ = false;
    target_width_ = 1920;
    target_height_ = 1080;
    cached_duration_us_ = 0;
    next_file_id_ = 1;
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
    last_resize_time_ = std::chrono::steady_clock::time_point{};
    stats_start_time_ = std::chrono::steady_clock::time_point{};
    for (auto& bl : perf_baselines_) bl.frames = 0;
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

void Renderer::play() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_ || playing_) return;
    if (seek_coordinator_) {
        seek_coordinator_->reset();
    }

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) continue;
        tracks_[i]->decode_thread->set_pause_after_preroll(false);
    }
    set_decode_paused_for_all_tracks(false);
    playback_->play();
    playing_ = true;
}

void Renderer::pause() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) continue;
        tracks_[i]->decode_thread->set_pause_after_preroll(true);
    }
    set_decode_paused_for_all_tracks(true);
    playback_->pause();
    playing_ = false;
}

void Renderer::seek(int64_t target_pts_us, SeekType type, int64_t request_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (request_id >= 0) {
        pending_seek_event_request_id_ = request_id;
        pending_seek_event_target_pts_us_ = target_pts_us;
        pending_seek_event_emitted_ = false;
    }
    seek_internal(target_pts_us, type);
}

void Renderer::set_loop_range(bool enabled, int64_t start_us, int64_t end_us) {
    const auto validation = validate_loop_range(enabled, start_us, end_us);
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid loop range: {}", validation.message);
        return;
    }

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    LoopRangeState next;
    if (!enabled || end_us <= start_us) {
        next = LoopRangeState();
    } else {
        next.enabled = true;
        next.start_us = std::max<int64_t>(0, start_us);
        next.end_us = std::max(next.start_us, end_us);
    }

    if (loop_range_.enabled == next.enabled &&
        loop_range_.start_us == next.start_us &&
        loop_range_.end_us == next.end_us) {
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

void Renderer::seek_internal(int64_t target_pts_us,
                             SeekType type,
                             bool allow_deferred,
                             bool force_recreate_paused_hevc) {
    // Caller must hold state_mutex_
    // See native/docs/SEEK_STRATEGY.md for codec/container-specific exact seek
    // limits, especially H.264 FLV streams without repeated SPS/PPS on IDR.
    const int64_t requested_pts_us = target_pts_us;
    const int64_t duration_us = effective_duration_us_locked();
    if (duration_us > 0) {
        target_pts_us = std::clamp(target_pts_us, int64_t(0), duration_us);
    } else {
        target_pts_us = std::max<int64_t>(0, target_pts_us);
    }
    if (target_pts_us != requested_pts_us) {
        spdlog::info("[Renderer] seek_internal clamp: requested={:.3f}s, clamped={:.3f}s, duration={:.3f}s",
                     requested_pts_us / 1e6,
                     target_pts_us / 1e6,
                     duration_us / 1e6);
    }
    if (pending_seek_event_request_id_ >= 0 &&
        !pending_seek_event_emitted_ &&
        pending_seek_event_target_pts_us_ == requested_pts_us) {
        pending_seek_event_target_pts_us_ = target_pts_us;
    }
    spdlog::info("[Renderer] seek_internal: target={:.3f}s, type={}",
                 target_pts_us / 1e6, type == SeekType::Exact ? "Exact" : "Keyframe");
    playback_->seek_clock(target_pts_us);
    if (allow_deferred && should_defer_paused_hevc_seek_locked(target_pts_us, type)) {
        return;
    }

    bool applied_seek = false;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) continue;
        auto* track = tracks_[i].get();
        if (type == SeekType::Exact && is_h264_flv_track(*track)) {
            spdlog::warn("[Renderer] Exact seek on H.264/FLV is best-effort: "
                         "streams that omit SPS/PPS on IDR frames can decode "
                         "incorrectly after seek. Remux/re-encode with repeated "
                         "headers for frame-accurate previews.");
        }
        const int64_t requested_track_target =
            std::max(target_pts_us - track->offset_us, int64_t(0));
        int64_t track_target =
            clamp_track_seek_target_us_locked(*track, target_pts_us);
        if (track_target != requested_track_target) {
            spdlog::info("[Renderer] seek_internal: track[{}] target clamp "
                         "requested={:.3f}s, clamped={:.3f}s",
                         i,
                         requested_track_target / 1e6,
                         track_target / 1e6);
        }
        // Pause decoder FIRST to prevent stale packets from reaching the codec
        // (avoids HEVC "Could not find ref" warnings during seek transition)
        track->decode_thread->set_decode_paused(true);
        if (audio_coordinator_) {
            audio_coordinator_->set_track_decode_paused(track->file_id, true);
        }
        auto buf_count_before = track->track_buffer->total_count();
        auto pq_size_before = track->packet_queue->size();
        const auto buffer_state_before = track->track_buffer->state();
        track->track_buffer->set_state(TrackState::Flushing);
        track->track_buffer->clear_frames();
        if (frame_presenter_ && track->decode_thread->is_hardware_decode_enabled()) {
            // Hardware seek invalidates the decoder surface epoch; software
            // upload textures stay reusable across seeks.
            frame_presenter_->reset_track(i);
        }
        track->packet_queue->flush();
        if (track->audio_packet_queue) {
            track->audio_packet_queue->flush();
        }
        const bool is_hevc_hw_seek =
            track->decode_thread->is_hardware_decode_enabled() &&
            track->decode_thread->codec_id() == AV_CODEC_ID_HEVC;
        const bool paused_seek = !playing_.load();
        const SeekType track_seek_type = type;
        const bool seek_transition_active =
            buffer_state_before == TrackState::Flushing ||
            buffer_state_before == TrackState::Buffering;
        const bool recreated_decode_only = false;
        const bool should_recreate_hevc_pipeline =
            is_hevc_hw_seek &&
            ((!paused_seek && !seek_transition_active) ||
             (paused_seek &&
              type != SeekType::Exact &&
              !track->recreated_for_paused_hevc_seek &&
              (!seek_transition_active || force_recreate_paused_hevc)));
        const bool recreated_for_seek =
            should_recreate_hevc_pipeline &&
            recreate_pipeline_for_seek(i, track_target, track_seek_type);
        if (is_hevc_hw_seek &&
            !paused_seek &&
            !seek_transition_active &&
            !recreated_decode_only &&
            !recreated_for_seek) {
            track->track_buffer->set_state(TrackState::Error);
            continue;
        }
        if (is_hevc_hw_seek && seek_transition_active) {
            spdlog::info("[Renderer] seek_internal: track[{}] coalescing HEVC HW seek during transition "
                         "(buf_state_before={}, target={:.3f}s)",
                         i, static_cast<int>(buffer_state_before), track_target / 1e6);
        }
        track = tracks_[i].get();
        track->decode_thread->set_pause_after_preroll(paused_seek);
        if (!recreated_decode_only && !recreated_for_seek) {
            track->seek_controller->request_seek(track_target, track_seek_type);
        }
        applied_seek = true;
        spdlog::info("[Renderer] seek_internal: track[{}] cleared (buf={}->{}, pq={}->0), state->Flushing, target={:.3f}s",
                     i, buf_count_before, track->track_buffer->total_count(), pq_size_before, track_target / 1e6);
    }
    if (applied_seek) {
        preview_drawn_ = false;
        last_decision_ = PresentDecision();
    }
}

bool Renderer::should_defer_paused_hevc_seek_locked(int64_t target_pts_us, SeekType type) {
    if (!seek_coordinator_) {
        return false;
    }

    const bool deferred = seek_coordinator_->should_defer_paused_hevc_seek(
        playing_.load(), has_hevc_hw_track_locked(), target_pts_us, type);
    if (deferred) {
        spdlog::info("[Renderer] Deferring paused HEVC HW seek to {:.3f}s", target_pts_us / 1e6);
    }
    return deferred;
}

bool Renderer::apply_deferred_paused_hevc_seek_locked() {
    if (!seek_coordinator_) {
        return false;
    }

    const auto deferred = seek_coordinator_->take_deferred_paused_hevc_seek(playing_.load());
    if (!deferred.has_value()) {
        return false;
    }
    spdlog::info("[Renderer] Applying deferred paused HEVC HW seek to {:.3f}s",
                 deferred->target_pts_us / 1e6);
    seek_internal(deferred->target_pts_us, deferred->type, false, true);
    return true;
}

bool Renderer::apply_loop_range_locked() {
    if (!playing_.load() ||
        !loop_range_.enabled ||
        loop_range_.end_us <= loop_range_.start_us ||
        playback_->clock().is_paused()) {
        return false;
    }

    const int64_t pts = playback_->clock().current_pts_us();
    if (pts < loop_range_.end_us) {
        return false;
    }

    spdlog::info("[Renderer] loop range boundary: pts={:.3f}s, seeking to {:.3f}s",
                 pts / 1e6, loop_range_.start_us / 1e6);
    seek_internal(loop_range_.start_us, SeekType::Exact);
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
    for (const auto& track : tracks_) {
        if (!track) continue;
        if (track->decode_thread->is_hardware_decode_enabled() &&
            track->decode_thread->codec_id() == AV_CODEC_ID_HEVC) {
            return true;
        }
    }
    return false;
}

void Renderer::emit_event(const RendererEvent& event) {
    RendererEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(event_callback_mutex_);
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

void Renderer::emit_seek_preview_presented_events(const PresentDecision& decision) {
    int64_t request_id = -1;
    int64_t target_pts_us = -1;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (pending_seek_event_request_id_ < 0 || pending_seek_event_emitted_) {
            return;
        }
        request_id = pending_seek_event_request_id_;
        target_pts_us = pending_seek_event_target_pts_us_;
        pending_seek_event_emitted_ = true;
    }

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!decision.frames[i].has_value()) continue;
        const int track_file_id = tracks_[i] ? tracks_[i]->file_id : -1;
        if (track_file_id < 0) continue;
        RendererEvent event;
        event.type = RendererEvent::Type::SeekPreviewPresented;
        event.request_id = request_id;
        event.track_file_id = track_file_id;
        event.pts_us = decision.frames[i]->pts_us;
        event.dts_us = decision.frames[i]->dts_us;
        event.target_pts_us = target_pts_us;
        emit_event(event);
    }
}

bool Renderer::build_step_forward_decision_locked(PresentDecision& decision) const {
    decision = PresentDecision();
    decision.current_pts_us = playback_->clock().current_pts_us();
    const int64_t frame_duration_us = compute_frame_duration_us();
    const int64_t max_step_gap_us = frame_duration_us + frame_duration_us / 2 + 2000;

    bool any_active = false;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) {
            decision.frames[i] = std::nullopt;
            continue;
        }
        any_active = true;

        int64_t base_pts = decision.current_pts_us - tracks_[i]->offset_us;
        if (last_decision_.frames[i].has_value()) {
            base_pts = last_decision_.frames[i]->pts_us;
        } else if (auto current = tracks_[i]->track_buffer->peek(0); current.has_value()) {
            base_pts = current->pts_us;
        }

        std::optional<TextureFrame> best;
        auto& buffer = tracks_[i]->track_buffer;
        const size_t total = buffer->total_count();
        for (size_t offset = 0; offset < total; ++offset) {
            auto frame = buffer->peek(static_cast<int>(offset));
            if (!frame.has_value() || frame->pts_us <= base_pts) {
                continue;
            }
            if (!best.has_value() || frame->pts_us < best->pts_us) {
                best = frame;
            }
        }

        if (!best.has_value()) {
            decision = PresentDecision();
            return false;
        }
        if (frame_duration_us > 0 && best->pts_us - base_pts > max_step_gap_us) {
            decision = PresentDecision();
            return false;
        }
        decision.frames[i] = best;
    }

    decision.should_present = any_active;
    return decision.should_present;
}

void Renderer::discard_step_forward_consumed_frames_locked(const PresentDecision& decision) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) continue;

        int64_t keep_after_pts = playback_->clock().current_pts_us() - tracks_[i]->offset_us;
        if (decision.frames[i].has_value()) {
            keep_after_pts = decision.frames[i]->pts_us;
        } else if (last_decision_.frames[i].has_value()) {
            keep_after_pts = last_decision_.frames[i]->pts_us;
        }

        auto& buffer = tracks_[i]->track_buffer;
        while (true) {
            auto frame = buffer->peek(0);
            if (!frame.has_value() || frame->pts_us > keep_after_pts) {
                break;
            }
            if (!buffer->advance()) {
                break;
            }
        }
    }
}

std::pair<float, float> Renderer::display_pixel_size_for_layout_locked(
    int width, int height, const LayoutState& layout) const {
    if (width <= 0 || height <= 0) {
        return {0.0f, 0.0f};
    }

    int active_count = 0;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) ++active_count;
    }
    if (active_count == 0) {
        return {static_cast<float>(width), static_cast<float>(height)};
    }

    int track_idx = -1;
    for (int display = 0; display < 4; ++display) {
        int candidate = layout.order[display];
        if (candidate >= 0 && candidate < static_cast<int>(kMaxTracks) && tracks_[candidate]) {
            track_idx = candidate;
            break;
        }
    }
    if (track_idx < 0) {
        for (int i = 0; i < static_cast<int>(kMaxTracks); ++i) {
            if (tracks_[i]) {
                track_idx = i;
                break;
            }
        }
    }
    if (track_idx < 0 || !tracks_[track_idx]) {
        return {static_cast<float>(width), static_cast<float>(height)};
    }

    float slot_w = static_cast<float>(width);
    float slot_h = static_cast<float>(height);
    if (layout.mode != LAYOUT_SPLIT_SCREEN && active_count > 1) {
        slot_w /= static_cast<float>(active_count);
    }
    const float slot_aspect = (slot_h > 0.0f) ? slot_w / slot_h : 1.0f;
    const float track_w = static_cast<float>(tracks_[track_idx]->video_width);
    const float track_h = static_cast<float>(tracks_[track_idx]->video_height);

    float track_scale = 1.0f;
    if (layout.pixel_size_mode == PIXEL_SIZE_UNIFORM_VIDEO_PIXELS) {
        int ref_idx = -1;
        int max_pixels = 0;
        for (int i = 0; i < static_cast<int>(kMaxTracks); ++i) {
            if (!tracks_[i]) continue;
            int pixels = tracks_[i]->video_width * tracks_[i]->video_height;
            if (pixels > max_pixels) {
                max_pixels = pixels;
                ref_idx = i;
            }
        }
        if (ref_idx < 0 || !tracks_[ref_idx]) {
            ref_idx = track_idx;
        }

        float ref_density = 1.0f;
        const float ref_w = static_cast<float>(tracks_[ref_idx]->video_width);
        const float ref_h = static_cast<float>(tracks_[ref_idx]->video_height);
        if (ref_w > 0.0f && ref_h > 0.0f) {
            ref_density = std::min(slot_w / ref_w, slot_h / ref_h);
        }

        float track_density = 1.0f;
        if (track_w > 0.0f && track_h > 0.0f) {
            track_density = std::min(slot_w / track_w, slot_h / track_h);
        }
        track_scale = (track_density > 0.0f) ? ref_density / track_density : 1.0f;
    }

    float video_aspect = tracks_[track_idx]->video_aspect;
    if (video_aspect <= 0.0f) {
        video_aspect = (track_h > 0.0f) ? track_w / track_h : slot_aspect;
    }
    if (video_aspect <= 0.0f) {
        video_aspect = slot_aspect;
    }

    float fit_scale = (video_aspect > slot_aspect)
        ? slot_aspect / video_aspect : 1.0f;
    fit_scale *= track_scale;
    const float display_scale = fit_scale * layout.zoom_ratio;
    const float ds_x = (slot_aspect > 0.0f)
        ? video_aspect * display_scale / slot_aspect : display_scale;
    const float ds_y = display_scale;

    return {ds_x * slot_w, ds_y * slot_h};
}

void Renderer::update_track_geometry_from_decision_locked(const PresentDecision& decision) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i] || !decision.frames[i].has_value()) continue;
        const TextureFrame& frame = decision.frames[i].value();
        if (frame.width <= 0 || frame.height <= 0) continue;

        auto& track = *tracks_[i];
        if (track.video_width == frame.width && track.video_height == frame.height) {
            continue;
        }

        float sar = 1.0f;
        if (track.video_width > 0 && track.video_height > 0 &&
            track.video_aspect > 0.0f) {
            const float old_natural_aspect =
                static_cast<float>(track.video_width) /
                static_cast<float>(track.video_height);
            if (old_natural_aspect > 0.0f) {
                sar = track.video_aspect / old_natural_aspect;
            }
        }
        if (!std::isfinite(sar) || sar <= 0.0f) {
            sar = 1.0f;
        }

        const int old_width = track.video_width;
        const int old_height = track.video_height;
        const float old_aspect = track.video_aspect;
        track.video_width = frame.width;
        track.video_height = frame.height;
        track.video_aspect =
            (static_cast<float>(frame.width) / static_cast<float>(frame.height)) * sar;
        if (!std::isfinite(track.video_aspect) || track.video_aspect <= 0.0f) {
            track.video_aspect =
                static_cast<float>(frame.width) / static_cast<float>(frame.height);
        }

        spdlog::info(
            "[Renderer] track[{}] display geometry changed: {}x{} aspect={:.6f} -> {}x{} aspect={:.6f}",
            i,
            old_width,
            old_height,
            old_aspect,
            track.video_width,
            track.video_height,
            track.video_aspect);
    }
}

void Renderer::step_forward() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    PresentDecision step_decision;
    bool have_step_decision = false;
    bool need_decode_wait = false;
    bool need_exact_seek = false;
    int64_t exact_seek_target = 0;

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_) return;

        // If any track is still seeking, don't step from a half-updated buffer.
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!tracks_[i]) continue;
            auto& buf = tracks_[i]->track_buffer;
            if (buf->state() == TrackState::Buffering) return;
        }

        playback_->clock().pause();
        playing_ = false;

        if (build_step_forward_decision_locked(step_decision)) {
            discard_step_forward_consumed_frames_locked(step_decision);
            int ref = first_active_track();
            if (ref >= 0) {
                auto& frame = step_decision.frames[ref];
                if (frame.has_value()) {
                    playback_->clock().seek(frame->pts_us + tracks_[ref]->offset_us);
                }
            }
            have_step_decision = true;
        } else {
            discard_step_forward_consumed_frames_locked(last_decision_);
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!tracks_[i]) continue;
                tracks_[i]->decode_thread->set_decode_paused(false);
            }
            need_decode_wait = true;
        }
    }

    if (need_decode_wait) {
        const auto deadline = std::chrono::steady_clock::now() + kStepForwardDecodeWait;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!initialized_) return;
                if (build_step_forward_decision_locked(step_decision)) {
                    for (size_t i = 0; i < kMaxTracks; ++i) {
                        if (!tracks_[i]) continue;
                        tracks_[i]->decode_thread->set_decode_paused(true);
                    }
                    discard_step_forward_consumed_frames_locked(step_decision);
                    int ref = first_active_track();
                    if (ref >= 0) {
                        auto& frame = step_decision.frames[ref];
                        if (frame.has_value()) {
                            playback_->clock().seek(frame->pts_us + tracks_[ref]->offset_us);
                        }
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
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!tracks_[i]) continue;
                tracks_[i]->decode_thread->set_decode_paused(true);
            }

            int64_t base_pts = playback_->clock().current_pts_us();
            int ref = first_active_track();
            if (ref >= 0) {
                if (last_decision_.frames[ref].has_value()) {
                    base_pts = last_decision_.frames[ref]->pts_us + tracks_[ref]->offset_us;
                } else if (auto frame = tracks_[ref]->track_buffer->peek(0); frame.has_value()) {
                    base_pts = frame->pts_us + tracks_[ref]->offset_us;
                }
            }
            int64_t dur = compute_frame_duration_us();
            exact_seek_target = base_pts + dur + 1000;
            if (cached_duration_us_ > 0) {
                exact_seek_target = std::min(exact_seek_target, cached_duration_us_);
            }
            spdlog::info("[Renderer] step_forward exact_seek: visible_pts={:.3f}s, clock_pts={:.3f}s, duration={:.3f}ms, target={:.3f}s",
                         base_pts / 1e6, playback_->clock().current_pts_us() / 1e6, dur / 1e3, exact_seek_target / 1e6);
            need_exact_seek = true;
        }
    }

    if (have_step_decision) {
        present_frame(step_decision);
        last_decision_ = step_decision;
        int ref = first_active_track();
        double pts = (ref >= 0 && step_decision.frames[ref].has_value())
                     ? step_decision.frames[ref]->pts_us / 1e6 : -1.0;
        spdlog::info("[Renderer] draw_paused_frame(step_forward): pts={:.3f}s", pts);
        return;
    }

    if (need_exact_seek) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        seek_internal(exact_seek_target, SeekType::Exact);
        spdlog::info("[Renderer] step_forward exact_seek done: clock_pts={:.3f}s",
                     playback_->clock().current_pts_us() / 1e6);
    }
}

void Renderer::step_backward() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!initialized_) return;

        // If any track is still seeking/seeking, don't allow stepping
        // (prevents retreating to stale frames during async seek)
        // Exception: tracks past their duration (Ready + no frames) don't block.
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!tracks_[i]) continue;
            auto& buf = tracks_[i]->track_buffer;
            if (buf->state() == TrackState::Buffering) return;
        }

        playback_->clock().pause();
        playing_ = false;

        // Check if ALL tracks can retreat (cache hit)
        bool all_can_retreat = true;
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!tracks_[i]) continue;
            if (!tracks_[i]->track_buffer->can_retreat()) {
                all_can_retreat = false;
                break;
            }
        }

        if (all_can_retreat) {
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!tracks_[i]) continue;
                tracks_[i]->track_buffer->retreat();
            }
            int ref = first_active_track();
            if (ref >= 0) {
                auto frame = tracks_[ref]->track_buffer->peek(0);
                if (frame.has_value()) {
                    playback_->clock().seek(frame->pts_us + tracks_[ref]->offset_us);
                }
            }
        } else {
            // Cache miss: exact seek to (current_pts - frame_duration - margin)
            // Add 1ms margin: frame duration is integer-truncated (e.g. 1/60s → 16666us)
            // but actual PTS spacing is 16667us, so (pts - dur) overshoots the
            // previous frame by 1us and exact seek's "< target" check discards it.
            int64_t dur = compute_frame_duration_us();
            int64_t target = std::max(int64_t(0),
                playback_->clock().current_pts_us() - dur - 1000);
            spdlog::info("[Renderer] step_backward exact_seek: pts={:.3f}s, duration={:.3f}ms, target={:.3f}s",
                         playback_->clock().current_pts_us() / 1e6, dur / 1e3, target / 1e6);
            seek_internal(target, SeekType::Exact);
            spdlog::info("[Renderer] step_backward exact_seek done: clock_pts={:.3f}s",
                         playback_->clock().current_pts_us() / 1e6);
            // Don't draw stale frame — seek_internal set preview_drawn_=false,
            // render loop will draw the new frame when decode completes.
            return;
        }
    }
    draw_paused_frame("step_backward");
}

void Renderer::draw_paused_frame(const char* reason) {
    PresentDecision decision;
    decision.current_pts_us = 0;
    decision.should_present = false;
    bool has_frame = false;
    for (size_t t = 0; t < kMaxTracks; ++t) {
        if (!tracks_[t]) {
            decision.frames[t] = std::nullopt;
            continue;
        }
        auto frame = tracks_[t]->track_buffer->peek(0);
        if (frame.has_value()) {
            decision.frames[t] = frame;
            has_frame = true;
        }
    }
    if (!has_frame && has_any_frame(last_decision_)) {
        decision = last_decision_;
        has_frame = true;
    }
    if (has_frame) {
        present_frame(decision);
        last_decision_ = decision;
        int ref = first_active_track();
        double pts = (ref >= 0 && decision.frames[ref].has_value())
                     ? decision.frames[ref]->pts_us / 1e6 : -1.0;
        spdlog::info("[Renderer] draw_paused_frame({}): pts={:.3f}s", reason, pts);
    }
}

void Renderer::wait_gpu_idle(const char* label) {
    const auto start = std::chrono::steady_clock::now();
    if (headless_output_) {
        headless_output_->wait_gpu_idle(label);
    } else if (d3d_device_) {
        d3d_device_->context()->Flush();
    }
    d3d_metrics_.render_wait_us.fetch_add(elapsed_us_since(start), std::memory_order_relaxed);
    d3d_metrics_.render_wait_count.fetch_add(1, std::memory_order_relaxed);
}

std::function<void()> Renderer::draw_headless_and_publish(const PresentDecision& decision, const char* label) {
    if (!headless_output_) {
        return {};
    }
    if (!d3d_resources_) {
        return {};
    }
    {
        std::lock_guard<std::mutex> tex_lock(texture_mutex());
        auto* rtv = headless_output_->begin_frame_locked();
        if (!rtv) {
            return {};
        }
        d3d_resources_->cached_rtv = rtv;
    }
    draw_frame(decision);
    const auto publish_start = std::chrono::steady_clock::now();
    headless_output_->wait_gpu_idle(label);
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> tex_lock(texture_mutex());
        callback = headless_output_->publish_frame_locked();
    }
    d3d_metrics_.present_publish_us.fetch_add(
        elapsed_us_since(publish_start), std::memory_order_relaxed);
    d3d_metrics_.present_publish_count.fetch_add(1, std::memory_order_relaxed);
    preview_drawn_ = true;
    return callback;
}

void Renderer::enter_terminal_device_lost_locked(const char* operation) {
    if (device_state_.load(std::memory_order_acquire) == RendererDeviceState::Terminal) {
        return;
    }

    device_state_.store(RendererDeviceState::Lost, std::memory_order_release);
    const long reason = d3d_device_
        ? static_cast<long>(d3d_device_->device_removed_reason())
        : static_cast<long>(S_OK);
    d3d_metrics_.device_lost_count.fetch_add(1, std::memory_order_relaxed);
    spdlog::error(
        "[Renderer] D3D11 device lost during {}; entering terminal renderer state "
        "(reason={:#x})",
        operation,
        static_cast<unsigned long>(reason));

    running_ = false;
    playing_ = false;
    playback_->pause();
    set_decode_paused_for_all_tracks(true);
    device_state_.store(RendererDeviceState::Terminal, std::memory_order_release);
}

void Renderer::present_frame(const PresentDecision& decision) {
    spdlog::debug("[present_frame] mode={}", layout_.mode);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        update_track_geometry_from_decision_locked(decision);
    }
    std::function<void()> frame_callback;
    bool device_lost = false;
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        if (headless_) {
            frame_callback = draw_headless_and_publish(decision, "present_frame");
            device_lost = d3d_device_ && d3d_device_->poll_device_removed("headless present");
        } else {
            draw_frame(decision);
            const auto present_start = std::chrono::steady_clock::now();
            const bool presented = d3d_device_->present(0);
            d3d_metrics_.present_publish_us.fetch_add(
                elapsed_us_since(present_start), std::memory_order_relaxed);
            d3d_metrics_.present_publish_count.fetch_add(1, std::memory_order_relaxed);
            device_lost = !presented && d3d_device_->device_lost();
            preview_drawn_ = true;
        }
    }
    if (device_lost) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_device_lost_locked("present_frame");
        return;
    }
    if (frame_callback) frame_callback();
}

void Renderer::redraw_layout() {
    std::function<void()> frame_callback;
    bool device_lost = false;
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        if (headless_) {
            frame_callback = draw_headless_and_publish(last_decision_, "redraw_layout");
            device_lost = d3d_device_ && d3d_device_->poll_device_removed("headless redraw");
        } else {
            draw_frame(last_decision_);
            d3d_device_->context()->Flush();
            device_lost = d3d_device_->poll_device_removed("redraw_layout");
            preview_drawn_ = true;
        }
    }
    if (device_lost) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_device_lost_locked("redraw_layout");
        return;
    }
    if (frame_callback) frame_callback();
}

bool Renderer::capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_ || !headless_output_) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    return frame_capture_.capture_headless_front_buffer(
        *headless_output_, device_mutex_, bgra, width, height);
}

bool Renderer::has_any_frame(const PresentDecision& decision) {
    for (auto& f : decision.frames) {
        if (f.has_value()) return true;
    }
    return false;
}

void Renderer::set_decode_paused_for_all_tracks(bool paused) {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) continue;
        tracks_[i]->decode_thread->set_decode_paused(paused);
    }
    if (audio_coordinator_) {
        audio_coordinator_->set_all_decode_paused(paused);
    }
}

void Renderer::configure_track_seek_callback(TrackPipeline& track) {
    auto* dt = track.decode_thread.get();
    const int file_id = track.file_id;
    track.demux_thread->set_seek_callback(
        [this, dt, file_id](int64_t pts, SeekType type) {
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
        track.file_id <= 0) {
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

int64_t Renderer::compute_frame_duration_us() const {
    // Use the minimum frame duration across all active tracks (= highest FPS).
    // This ensures step_backward moves in the finest granularity, so the fastest
    // track always advances exactly 1 frame; slower tracks hold until they have
    // a frame at the target PTS.
    int64_t min_dur = INT64_MAX;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) continue;
        auto frame = tracks_[i]->track_buffer->peek(0);
        if (frame.has_value() && frame->duration_us > 0) {
            min_dur = std::min(min_dur, frame->duration_us);
        }
    }
    if (min_dur != INT64_MAX && min_dur <= 100000) {
        return min_dur;
    }
    return 33333; // fallback ~30fps
}

int64_t Renderer::clamp_track_seek_target_us_locked(
    const TrackPipeline& track,
    int64_t target_pts_us) const {
    int64_t track_target = std::max(target_pts_us - track.offset_us, int64_t(0));
    if (!track.demux_thread) {
        return track_target;
    }

    const auto stats = track.demux_thread->stats();
    const int64_t track_end_us = track_pts_end_us_from_stats(stats);
    if (track_end_us <= 0) {
        return track_target;
    }

    return std::min(track_target, track_end_us);
}

int64_t Renderer::effective_duration_us_locked() const {
    int64_t duration_us = 0;
    bool has_track_duration = false;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i] || !tracks_[i]->demux_thread) continue;
        const int64_t track_pts_end_us =
            track_pts_end_us_from_stats(tracks_[i]->demux_thread->stats());
        if (track_pts_end_us <= 0) continue;
        has_track_duration = true;
        duration_us = std::max(
            duration_us,
            track_pts_end_us + tracks_[i]->offset_us);
    }
    if (!has_track_duration) {
        duration_us = cached_duration_us_;
    }
    return std::max<int64_t>(0, duration_us);
}

bool Renderer::settle_eof_locked(int64_t max_presented_end_us) {
    if (!playing_.load() || max_presented_end_us <= 0) {
        return false;
    }

    const int64_t duration_us = effective_duration_us_locked();
    const int64_t current_us = playback_->clock().current_pts_us();
    const int64_t frame_duration_us = compute_frame_duration_us();
    const int64_t eof_tolerance_us =
        std::max<int64_t>(frame_duration_us + 2000, 5000);

    int64_t end_us = max_presented_end_us;
    if (duration_us > 0) {
        if (std::llabs(duration_us - max_presented_end_us) > eof_tolerance_us) {
            return false;
        }
        end_us = duration_us;
    }

    if (current_us + eof_tolerance_us < end_us) {
        return false;
    }

    set_decode_paused_for_all_tracks(true);
    playback_->clock().seek(end_us);
    playback_->clock().pause();
    playing_ = false;
    preview_drawn_ = true;
    spdlog::info("[Renderer] EOF reached: clock fixed at {:.3f}s (last_frame_end={:.3f}s, duration={:.3f}s)",
                 end_us / 1e6,
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
    size_t count = 0;
    for (const auto& t : tracks_) { if (t) ++count; }
    return count;
}

int64_t Renderer::duration_us() const {
    return cached_duration_us_;
}

void Renderer::set_track_offset(int file_id, int64_t offset_us) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    int slot = find_slot_by_file_id(file_id);
    if (slot < 0 || !tracks_[slot]) return;
    tracks_[slot]->offset_us = offset_us;
    render_sink_->set_track_offset(slot, offset_us);
    preview_drawn_ = false;
}

void Renderer::set_frame_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (headless_output_) {
        headless_output_->set_frame_callback(std::move(cb));
    }
}

void Renderer::set_event_callback(RendererEventCallback cb) {
    std::lock_guard<std::mutex> lock(event_callback_mutex_);
    event_callback_ = std::move(cb);
}

bool Renderer::acquire_shared_texture(SharedTextureSnapshot& snapshot) const {
    snapshot = {};

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_output_) {
        d3d_metrics_.texture_sharing_failure_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::lock_guard<std::mutex> lock(texture_mutex());
    D3D11HeadlessOutputTextureLease lease;
    if (!headless_output_->acquire_shared_texture_locked(lease)) {
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
}

void Renderer::release_shared_texture(int buffer_index, uint64_t buffer_generation) const {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (headless_output_) {
        headless_output_->release_shared_texture(buffer_index, buffer_generation);
    }
}

std::mutex& Renderer::texture_mutex() const {
    return headless_output_ ? headless_output_->texture_mutex() : texture_mutex_fallback_;
}

void Renderer::resize(int width, int height) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_ || !d3d_device_) return;
    const auto validation = validate_renderer_dimensions(width, height, "resize dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid resize: {}", validation.message);
        return;
    }
    pending_width_.store(width);
    pending_height_.store(height);
}

void Renderer::do_resize(int width, int height) {
    if (width == target_width_ && height == target_height_) return;

    spdlog::info("[Renderer] resize: {}x{} -> {}x{}", target_width_, target_height_, width, height);

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto old_display = display_pixel_size_for_layout_locked(
            target_width_, target_height_, layout_);
        const auto new_display = display_pixel_size_for_layout_locked(width, height, layout_);
        if (old_display.first > 1e-4f && new_display.first > 1e-4f) {
            layout_.view_offset[0] *= new_display.first / old_display.first;
        }
        if (old_display.second > 1e-4f && new_display.second > 1e-4f) {
            layout_.view_offset[1] *= new_display.second / old_display.second;
        }
    }

    std::function<void()> frame_callback;
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        {
            std::lock_guard<std::mutex> tex_lock(texture_mutex());
            if (!headless_output_ || !headless_output_->resize_locked(width, height)) {
                return;
            }
        }
        d3d_metrics_.shared_texture_resize_count.fetch_add(1, std::memory_order_relaxed);

        target_width_ = width;
        target_height_ = height;

        frame_callback = draw_headless_and_publish(last_decision_, "resize");
    }
    if (frame_callback) frame_callback();
}

void Renderer::render_loop() {
    // Raise Windows timer resolution from default ~15.6ms to 1ms,
    // so sleep_for(16ms) actually wakes up near 16ms instead of 31ms.
    timeBeginPeriod(1);
    spdlog::info("[Renderer] Render loop started (timer resolution: 1ms), tid={}", GetCurrentThreadId());

    // Periodic diagnostics — log buffer state every 2 seconds
    auto diag_time = std::chrono::steady_clock::now();
    int64_t diag_last_pts = 0;
    constexpr auto diag_interval = std::chrono::seconds(2);

    while (running_) {
        if (d3d_device_ && d3d_device_->poll_device_removed("render_loop")) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            enter_terminal_device_lost_locked("render_loop");
            break;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            if (apply_deferred_paused_hevc_seek_locked()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (apply_loop_range_locked()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
        }

        // Process pending resize (debounced — at most ~30Hz).
        {
            int pw = pending_width_.exchange(0);
            int ph = pending_height_.exchange(0);
            if (pw > 0 && ph > 0) {
                auto now = std::chrono::steady_clock::now();
                if (now - last_resize_time_ >= std::chrono::milliseconds(33)) {
                    do_resize(pw, ph);
                    last_resize_time_ = now;
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

        if (headless_output_) {
            headless_output_->cleanup_expired_pending_buffers();
        }

        // Snapshot playing_ under state_mutex_ to avoid torn read
        // when pause()/resume() modify it concurrently.
        bool playing_snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            playing_snapshot = playing_;
        }

        // Preroll: keep clock paused while any track is still buffering
        bool any_buffering = false;
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!tracks_[i]) continue;
            auto buf_state = tracks_[i]->track_buffer->state();
            if (buf_state == TrackState::Buffering ||
                buf_state == TrackState::Empty ||
                buf_state == TrackState::Flushing) {
                any_buffering = true;
                break;
            }
        }

        // Detect Buffering → Ready transition: force preview redraw so the
        // newly-ready track's first frame appears on screen immediately
        // (even while paused — matches initialize() behavior).
        if (was_buffering_ && !any_buffering) {
            preview_drawn_ = false;
            last_decision_ = PresentDecision();  // Clear stale cached frames
            spdlog::info("[Renderer] Preroll transition complete, forcing preview redraw");
        }
        was_buffering_ = any_buffering;

        if (any_buffering && !playback_->clock().is_paused()) {
            playback_->clock().pause();
            spdlog::info("[Renderer] Preroll: clock PENDING, some track buffering, "
                         "(playing={})", playing_snapshot);
        } else if (!any_buffering && playback_->clock().is_paused() && playing_snapshot) {
            set_decode_paused_for_all_tracks(false);
            playback_->clock().resume();
            preview_drawn_ = false;
            spdlog::info("[Renderer] === Preroll COMPLETE: all tracks ready, clock resumed, "
                         "playing_={}, pts={:.3f}s)",
                         playing_snapshot, playback_->clock().current_pts_us() / 1e6);
        }

        if (!playing_snapshot || playback_->clock().is_paused()) {
            // While paused/prerolling, draw current frame if not yet drawn
            if (!preview_drawn_) {
                bool drawn = false;

                // Try cached last frame first (for layout changes while paused)
                if (has_any_frame(last_decision_)) {
                    present_frame(last_decision_);
                    drawn = true;
                    spdlog::debug("[Renderer] Paused frame (cached): pts={:.3f}s",
                                 [&]{
                                     for (auto& f : last_decision_.frames)
                                         if (f.has_value()) return f->pts_us / 1e6;
                                     return -1.0;
                                 }());
                }

                // No cached frame — try track buffer (initial preview)
                // Only draw when ALL active tracks have frames, to avoid
                // flashing black for tracks that haven't finished seeking.
                if (!drawn) {
                    PresentDecision preview;
                    preview.current_pts_us = 0;
                    preview.should_present = false;
                    bool all_active_have_frames = true;
                    bool all_active_ready = true;
                    for (size_t t = 0; t < kMaxTracks; ++t) {
                        if (!tracks_[t]) continue;
                        const auto state = tracks_[t]->track_buffer->state();
                        if (state != TrackState::Ready) {
                            all_active_ready = false;
                        }
                        auto frame = tracks_[t]->track_buffer->peek(0);
                        if (frame.has_value()) {
                            preview.frames[t] = frame;
                        } else if (state == TrackState::Ready) {
                            // Track is Ready but has no frames — past its duration (EOF).
                            // Don't block preview drawing for other tracks.
                        } else {
                            all_active_have_frames = false;
                        }
                    }
                    if (all_active_ready && all_active_have_frames && has_any_frame(preview)) {
                        present_frame(preview);
                        last_decision_ = preview;
                        bool preserve_requested_clock = false;
                        if (!playing_snapshot) {
                            set_decode_paused_for_all_tracks(true);
                            std::lock_guard<std::mutex> lock(state_mutex_);
                            preserve_requested_clock = true;
                            mark_paused_hevc_seek_preview_drawn_locked();
                        }
                        // Keep the logical clock at the user's requested target
                        // while paused. The decoded preview can land on a
                        // nearest/tail frame for individual tracks, but the
                        // timeline should not visually snap backward.
                        int ref = first_active_track();
                        if (!preserve_requested_clock &&
                            ref >= 0 &&
                            preview.frames[ref].has_value()) {
                            playback_->clock().seek(
                                preview.frames[ref]->pts_us + tracks_[ref]->offset_us);
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

        // Periodic diagnostics
        {
            auto now = std::chrono::steady_clock::now();
            if (now - diag_time >= diag_interval) {
                diag_time = now;
                int64_t pts = playback_->clock().current_pts_us();
                int64_t pts_delta = pts - diag_last_pts;
                diag_last_pts = pts;
                for (size_t i = 0; i < kMaxTracks; ++i) {
                    if (!tracks_[i]) continue;
                    auto buf_count = tracks_[i]->track_buffer->total_count();
                    auto buf_cap = tracks_[i]->track_buffer->preroll_target();
                    auto buf_state = tracks_[i]->track_buffer->state();
                    spdlog::info("[diag] track[{}]: pts={:.3f}s delta={:.1f}ms "
                                 "buf={}/{} state={} playing={}",
                                 i, pts / 1e6, pts_delta / 1e3,
                                 buf_count, buf_cap,
                                 static_cast<int>(buf_state), playing_snapshot);
                }
            }
        }

        if (decision.should_present) {
            // Independent presentation: fill missing tracks from last decision
            // so each track always shows a frame (new or carried over).
            // Once a track has started, keep carrying its last frame even after
            // that track reaches EOF. This lets shorter tracks freeze on their
            // final image while longer tracks continue playing.
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!decision.frames[i].has_value() &&
                    last_decision_.frames[i].has_value() && tracks_[i]) {
                    int64_t effective_pts = decision.current_pts_us - tracks_[i]->offset_us;
                    if (effective_pts >= 0) {
                        decision.frames[i] = last_decision_.frames[i];
                    }
                }
            }
            present_frame(decision);
            last_decision_ = decision;
        } else if (!preview_drawn_) {
            // No new frame but layout changed (e.g. zoom/pan during playback)
            if (has_any_frame(last_decision_)) {
                redraw_layout();
            }
        }

        // Frame-driven clock: when buffer is empty, clamp clock to the
        // end of the last presented frame so PTS doesn't run ahead.
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            bool buffer_empty = true;
            int64_t max_end_pts = 0;
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!tracks_[i]) continue;
                if (tracks_[i]->track_buffer->peek(0).has_value()) {
                    buffer_empty = false;
                    // No need to check further — one non-empty buffer is enough
                    break;
                }
                if (last_decision_.frames[i].has_value()) {
                    max_end_pts = std::max(max_end_pts,
                        last_decision_.frames[i]->pts_us +
                        last_decision_.frames[i]->duration_us +
                        tracks_[i]->offset_us);
                }
            }
            if (buffer_empty && max_end_pts > 0) {
                int64_t current = playback_->clock().current_pts_us();
                if (current > max_end_pts) {
                    playback_->clock().seek(max_end_pts);
                }
                if (settle_eof_locked(max_end_pts)) {
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
            int64_t next_event_pts = INT64_MAX;

            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!tracks_[i]) continue;
                auto frame = tracks_[i]->track_buffer->peek(0);
                if (!frame.has_value()) continue;
                if (frame->pts_us > current_pts) {
                    // Future frame — wake when it should start
                    next_event_pts = std::min(next_event_pts, frame->pts_us);
                } else {
                    // Frame being displayed — wake when it expires
                    next_event_pts = std::min(next_event_pts,
                                              frame->pts_us + frame->duration_us);
                }
            }

            if (next_event_pts != INT64_MAX) {
                double spd = playback_->clock().speed();
                if (spd > 0) {
                    int64_t pts_delta = next_event_pts - current_pts;
                    int64_t sleep_us = static_cast<int64_t>(pts_delta / spd);
                    if (sleep_us > 0) {
                        if (sleep_us > MAX_SLEEP_US) sleep_us = MAX_SLEEP_US;
                        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
                    }
                }
            } else {
                // No frames available (buffer underflow) — short poll
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    // Flush any pending resize before exiting.
    {
        int pw = pending_width_.exchange(0);
        int ph = pending_height_.exchange(0);
        if (pw > 0 && ph > 0) do_resize(pw, ph);
    }

    spdlog::info("[Renderer] Render loop ended");
    timeEndPeriod(1);
}

void Renderer::draw_frame(const PresentDecision& decision) {
    if (!d3d_resources_) {
        return;
    }
    auto& resources = *d3d_resources_;
    auto* ctx = d3d_device_->context();

    // Get or create cached render target view
    if (!resources.cached_rtv) {
        if (!headless_) {
            ID3D11Texture2D* back_buffer = nullptr;
            HRESULT hr = d3d_device_->swap_chain()->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                                  reinterpret_cast<void**>(&back_buffer));
            if (FAILED(hr)) {
                spdlog::error("[Renderer] Failed to get back buffer: HRESULT {:#x}", static_cast<unsigned long>(hr));
                return;
            }
            hr = d3d_device_->device()->CreateRenderTargetView(
                back_buffer, nullptr, &resources.cached_rtv);
            back_buffer->Release();
            if (FAILED(hr)) {
                spdlog::error("[Renderer] Failed to create RTV: HRESULT {:#x}", static_cast<unsigned long>(hr));
                return;
            }
        }
    }

    if (!resources.cached_rtv) {
        return;
    }

    float clear_color[4] = {};
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (int i = 0; i < 4; ++i) {
            clear_color[i] = background_color_[i];
        }
    }
    ctx->ClearRenderTargetView(resources.cached_rtv.Get(), clear_color);
    ctx->OMSetRenderTargets(1, resources.cached_rtv.GetAddressOf(), nullptr);

    // Setup viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(target_width_);
    vp.Height = static_cast<float>(target_height_);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Setup input assembler
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    ID3D11Buffer* vb = resources.vertex_buffer.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    if (resources.compiled_shader.layout) {
        ctx->IASetInputLayout(resources.compiled_shader.layout.Get());
    }

    // Set shaders
    ctx->VSSetShader(resources.compiled_shader.vs.Get(), nullptr, 0);
    ctx->PSSetShader(resources.compiled_shader.ps.Get(), nullptr, 0);

    ID3D11ShaderResourceView* srvs[4] = {};           // t0-t3: RGBA
    ID3D11ShaderResourceView* nv12_y_srvs[4] = {};    // t4-t7: NV12 Y or planar Y
    ID3D11ShaderResourceView* nv12_uv_srvs[4] = {};   // t8-t11: NV12 UV
    ID3D11ShaderResourceView* planar_u_srvs[4] = {};  // t12-t15: planar U
    ID3D11ShaderResourceView* planar_v_srvs[4] = {};  // t16-t19: planar V
    std::array<D3D11PreparedFrame, kMaxTracks> prepared_frames;
    if (frame_presenter_) {
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!decision.frames[i].has_value() || !decision.frames[i]->texture_handle) continue;
            if (!tracks_[i]) continue;

            const auto prepare_start = std::chrono::steady_clock::now();
            const bool prepared_ok = frame_presenter_->prepare_frame(
                i,
                decision.frames[i].value(),
                target_width_,
                target_height_,
                [this](const char* label) { wait_gpu_idle(label); },
                prepared_frames[i]);
            d3d_metrics_.frame_copy_us.fetch_add(
                elapsed_us_since(prepare_start), std::memory_order_relaxed);
            d3d_metrics_.frame_copy_count.fetch_add(1, std::memory_order_relaxed);
            if (!prepared_ok) {
                continue;
            }

            srvs[i] = prepared_frames[i].rgba_srv;
            nv12_y_srvs[i] = prepared_frames[i].nv12_y_srv;
            nv12_uv_srvs[i] = prepared_frames[i].nv12_uv_srv;
            planar_u_srvs[i] = prepared_frames[i].planar_u_srv;
            planar_v_srvs[i] = prepared_frames[i].planar_v_srv;
        }
    }

    ShaderConstants cb = {};
    bool constants_ready = false;

    // Update constant buffer
    // Layout must match HLSL cbuffer Constants in multitrack.hlsl
    if (resources.compiled_shader.constant_buffer) {
        // Snapshot layout state atomically
        LayoutState snap;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            snap = layout_;
        }

        cb.mode = snap.mode;
        cb.split_pos = snap.split_pos;
        cb.zoom_ratio = snap.zoom_ratio;
        cb.canvas_width = static_cast<float>(target_width_);
        cb.canvas_height = static_cast<float>(target_height_);
        cb.view_offset[0] = snap.view_offset[0];
        cb.view_offset[1] = snap.view_offset[1];
        cb.nv12_mask = 0;
        cb.planar_yuv_mask = 0;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            for (int i = 0; i < 4; ++i) {
                cb.background_color[i] = background_color_[i];
            }
        }
        for (int i = 0; i < 4; ++i) {
            cb.order[i] = snap.order[i];
        }
        int active_count = 0;
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!tracks_[i]) {
                cb.video_aspect[i] = 1.0f;
                cb.nv12_uv_scale_x[i] = 1.0f;
                cb.nv12_uv_scale_y[i] = 1.0f;
                cb.color_range[i] = VIDEO_COLOR_RANGE_LIMITED;
                cb.color_matrix[i] = VIDEO_COLOR_MATRIX_BT709;
                cb.color_transfer[i] = VIDEO_COLOR_TRANSFER_SDR;
                cb.color_primaries[i] = VIDEO_COLOR_PRIMARIES_BT709;
                continue;
            }
            ++active_count;
            cb.video_aspect[i] = tracks_[i]->video_aspect;
            const VideoColorInfo color = decision.frames[i].has_value()
                ? decision.frames[i]->color
                : VideoColorInfo{};
            cb.color_range[i] = color.range != VIDEO_COLOR_RANGE_UNKNOWN
                ? color.range
                : VIDEO_COLOR_RANGE_LIMITED;
            cb.color_matrix[i] = color.matrix != VIDEO_COLOR_MATRIX_UNKNOWN
                ? color.matrix
                : (tracks_[i]->video_width >= 1280 || tracks_[i]->video_height > 576
                    ? VIDEO_COLOR_MATRIX_BT709
                    : VIDEO_COLOR_MATRIX_BT601);
            cb.color_transfer[i] = color.transfer != VIDEO_COLOR_TRANSFER_UNKNOWN
                ? color.transfer
                : VIDEO_COLOR_TRANSFER_SDR;
            cb.color_primaries[i] = color.primaries != VIDEO_COLOR_PRIMARIES_UNKNOWN
                ? color.primaries
                : (cb.color_matrix[i] == VIDEO_COLOR_MATRIX_BT2020_NCL
                    ? VIDEO_COLOR_PRIMARIES_BT2020
                    : (cb.color_matrix[i] == VIDEO_COLOR_MATRIX_BT601
                        ? VIDEO_COLOR_PRIMARIES_BT601
                        : VIDEO_COLOR_PRIMARIES_BT709));
            if (decision.frames[i].has_value() &&
                decision.frames[i]->cpu_planar_yuv_storage()) {
                cb.planar_yuv_mask |= (1 << static_cast<int>(i));
                cb.nv12_uv_scale_x[i] = 1.0f;
                cb.nv12_uv_scale_y[i] = 1.0f;
            } else if (decision.frames[i].has_value() && decision.frames[i]->is_nv12) {
                cb.nv12_mask |= (1 << static_cast<int>(i));
                cb.nv12_uv_scale_x[i] = frame_presenter_
                    ? frame_presenter_->nv12_uv_scale_x(i)
                    : 1.0f;
                cb.nv12_uv_scale_y[i] = frame_presenter_
                    ? frame_presenter_->nv12_uv_scale_y(i)
                    : 1.0f;
            } else {
                cb.nv12_uv_scale_x[i] = frame_presenter_
                    ? frame_presenter_->nv12_uv_scale_x(i)
                    : 1.0f;
                cb.nv12_uv_scale_y[i] = frame_presenter_
                    ? frame_presenter_->nv12_uv_scale_y(i)
                    : 1.0f;
            }
        }
        cb.track_count = active_count;

        // Compute per-track scale for the selected pixel-size policy.
        {
            for (int i = 0; i < 4; ++i) {
                cb.track_scale[i] = 1.0f;
            }

            if (snap.pixel_size_mode == PIXEL_SIZE_UNIFORM_VIDEO_PIXELS) {
                int ref_idx = -1;
                int max_pixels = 0;
                for (int i = 0; i < 4; ++i) {
                    if (!tracks_[i]) continue;
                    int pixels = tracks_[i]->video_width * tracks_[i]->video_height;
                    if (pixels > max_pixels) {
                        max_pixels = pixels;
                        ref_idx = i;
                    }
                }
                if (ref_idx < 0) ref_idx = 0;

                // Slot dimensions depend on layout mode
                float slot_w = static_cast<float>(target_width_);
                float slot_h = static_cast<float>(target_height_);
                if (snap.mode != LAYOUT_SPLIT_SCREEN && active_count > 1) {
                    slot_w /= static_cast<float>(active_count);
                }

                // Reference video density: min(slot_w / ref_w, slot_h / ref_h)
                float ref_density = 1.0f;
                if (tracks_[ref_idx]) {
                    float ref_w = static_cast<float>(tracks_[ref_idx]->video_width);
                    float ref_h = static_cast<float>(tracks_[ref_idx]->video_height);
                    if (ref_w > 0.0f && ref_h > 0.0f) {
                        ref_density = std::min(slot_w / ref_w, slot_h / ref_h);
                    }
                }

                for (int i = 0; i < 4; ++i) {
                    if (!tracks_[i]) continue;
                    float tw = static_cast<float>(tracks_[i]->video_width);
                    float th = static_cast<float>(tracks_[i]->video_height);
                    float density = 1.0f;
                    if (tw > 0.0f && th > 0.0f) {
                        density = std::min(slot_w / tw, slot_h / th);
                    }
                    cb.track_scale[i] = (density > 0.0f) ? ref_density / density : 1.0f;
                }
            }
        }

        // Precompute per-track display constants (moves heavy math from pixel shader to CPU)
        {
            float slot_w = static_cast<float>(target_width_);
            float slot_h = static_cast<float>(target_height_);
            if (snap.mode != LAYOUT_SPLIT_SCREEN && active_count > 1) {
                slot_w /= static_cast<float>(active_count);
            }
            float slot_aspect = (slot_h > 0.0f) ? slot_w / slot_h : 1.0f;

            for (int i = 0; i < 4; ++i) {
                float video_aspect = cb.video_aspect[i];
                if (!std::isfinite(video_aspect) || video_aspect <= 0.0f) {
                    video_aspect = slot_aspect;
                }

                // Aspect-fit scale
                float fit_scale = (video_aspect > slot_aspect)
                    ? slot_aspect / video_aspect : 1.0f;
                fit_scale *= cb.track_scale[i];
                if (!std::isfinite(fit_scale) || fit_scale <= 0.0f) {
                    fit_scale = 1.0f;
                }

                // Apply zoom
                float display_scale = fit_scale * snap.zoom_ratio;
                if (!std::isfinite(display_scale) || display_scale <= 0.0f) {
                    display_scale = 1.0f;
                }

                // Display size in slot UV space
                float ds_x = (slot_aspect > 0.0f)
                    ? video_aspect * display_scale / slot_aspect : display_scale;
                float ds_y = display_scale;

                // Display offset (centering)
                cb.display_offset_x[i] = (1.0f - ds_x) * 0.5f;
                cb.display_offset_y[i] = (1.0f - ds_y) * 0.5f;

                // Inverse display size (for fast division in shader)
                cb.inv_display_size_x[i] = (fabsf(ds_x) > 1e-4f) ? 1.0f / ds_x : 0.0f;
                cb.inv_display_size_y[i] = (fabsf(ds_y) > 1e-4f) ? 1.0f / ds_y : 0.0f;

                // View offset in video UV space
                float dp_x = ds_x * slot_w;
                float dp_y = ds_y * slot_h;
                cb.view_offset_uv_x[i] = (fabsf(dp_x) > 1e-4f) ? snap.view_offset[0] / dp_x : 0.0f;
                cb.view_offset_uv_y[i] = (fabsf(dp_y) > 1e-4f) ? snap.view_offset[1] / dp_y : 0.0f;
            }
        }
        ctx->UpdateSubresource(resources.compiled_shader.constant_buffer.Get(), 0, nullptr, &cb, 0, 0);
        ctx->VSSetConstantBuffers(0, 1, resources.compiled_shader.constant_buffer.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, resources.compiled_shader.constant_buffer.GetAddressOf());
        constants_ready = true;
    }

    // Set sampler
    if (resources.sampler_state) {
        ID3D11SamplerState* sampler = resources.sampler_state.Get();
        ctx->PSSetSamplers(0, 1, &sampler);
    }

    // Bind SRVs: t0-t3 RGBA, t4-t7 Y, t8-t11 NV12 UV, t12-t15 planar U, t16-t19 planar V
    ctx->PSSetShaderResources(0, 4, srvs);
    ctx->PSSetShaderResources(4, 4, nv12_y_srvs);
    ctx->PSSetShaderResources(8, 4, nv12_uv_srvs);
    ctx->PSSetShaderResources(12, 4, planar_u_srvs);
    ctx->PSSetShaderResources(16, 4, planar_v_srvs);

    // Draw
    ctx->Draw(4, 0);

    if (constants_ready && analysis_overlay_renderer_) {
        analysis_overlay_renderer_->draw(
            decision,
            tracks_,
            *d3d_device_,
            *d3d_resources_,
            target_width_,
            target_height_);
    }

    // Unbind SRVs before releasing to avoid GPU resource-in-use issues
    ID3D11ShaderResourceView* null_srvs[4] = {};
    ctx->PSSetShaderResources(0, 4, null_srvs);
    ctx->PSSetShaderResources(4, 4, null_srvs);
    ctx->PSSetShaderResources(8, 4, null_srvs);
    ctx->PSSetShaderResources(12, 4, null_srvs);
    ctx->PSSetShaderResources(16, 4, null_srvs);

    // Temporary direct-texture SRVs are owned by prepared_frames until draw returns.
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
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) return static_cast<int>(i);
    }
    return -1;
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
    return tracks_.create_pipeline(path, hw_decode, initial_seek);
}

bool Renderer::recreate_pipeline_for_seek(size_t slot, int64_t target_pts_us, SeekType type) {
    auto& current = tracks_[slot];
    if (!current) {
        return false;
    }

    spdlog::info("[Renderer] Recreating pipeline for {}", current->file_path);

    const auto file_path = current->file_path;
    const auto file_id = current->file_id;
    const auto offset_us = current->offset_us;
    const auto use_hardware_decode = current->use_hardware_decode;

    unregister_track_audio(file_id);
    render_sink_->set_track(slot, nullptr);
    if (frame_presenter_) {
        frame_presenter_->reset_track(slot);
    }
    tracks_.stop_slot(slot);

    // Give the driver a brief moment to retire the previous D3D11VA decoder
    // objects before constructing a fresh hardware pipeline on the same file.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    const SeekRequest initial_seek{target_pts_us, type};
    auto replacement = create_pipeline(file_path, use_hardware_decode, &initial_seek);
    if (!replacement) {
        spdlog::error("[Renderer] Failed to recreate pipeline for {}", file_path);
        return false;
    }

    replacement->file_id = file_id;
    replacement->offset_us = offset_us;
    replacement->recreated_for_paused_hevc_seek = true;
    configure_track_seek_callback(*replacement);
    configure_track_error_callback(*replacement);
    register_track_audio(*replacement);
    if (!replacement->demux_thread->start_thread()) {
        spdlog::error("[Renderer] Failed to start recreated demux thread for {}", file_path);
        unregister_track_audio(file_id);
        replacement->decode_thread->stop();
        replacement->demux_thread->stop();
        return false;
    }

    render_sink_->set_track(slot, replacement->track_buffer);
    render_sink_->set_track_offset(slot, offset_us);
    tracks_[slot] = std::move(replacement);
    return true;
}

bool Renderer::recreate_decode_thread_for_seek(size_t slot, int64_t target_pts_us, SeekType type) {
    auto& track = tracks_[slot];
    if (!track) {
        return false;
    }

    spdlog::info("[Renderer] Recreating decode thread for paused seek on {}", track->file_path);

    track->decode_thread->stop();
    track->packet_queue->reset();
    track->packet_queue->flush();
    track->track_buffer->reset();
    track->track_buffer->set_state(TrackState::Flushing);
    if (frame_presenter_) {
        frame_presenter_->reset_track(slot);
    }

    const auto& stats = track->demux_thread->stats();
    auto replacement = std::make_unique<DecodeThread>(
        *track->packet_queue, *track->track_buffer, stats.codec_params, stats.time_base);
    if (!replacement->is_valid()) {
        spdlog::error("[Renderer] Failed to recreate decode thread for {}", track->file_path);
        return false;
    }

    const int file_id = track->file_id;
    track->demux_thread->set_seek_callback(
        [this, dt = replacement.get(), file_id](int64_t pts, SeekType seek_type) {
            dt->notify_seek(pts, seek_type);
            if (audio_coordinator_) {
                audio_coordinator_->notify_seek(file_id, pts, seek_type);
            }
        });

    if (track->use_hardware_decode) {
        replacement->enable_hardware_decode(
            default_decode_device_mode(stats.codec_params->codec_id));
    }

    if (!replacement->start()) {
        spdlog::error("[Renderer] Failed to start recreated decode thread for {}", track->file_path);
        return false;
    }

    track->decode_thread = std::move(replacement);
    track->seek_controller->request_seek(target_pts_us, type);
    return true;
}

int Renderer::add_track(const std::string& video_path,
                        bool use_hardware_decode) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_) return -1;

    int slot = find_empty_slot();
    if (slot < 0) {
        spdlog::warn("Renderer::add_track: no empty slots");
        return -1;
    }

    // Pause playback to avoid render loop reading partially-initialized pipeline
    bool was_playing = playing_.load();
    if (was_playing) {
        playback_->pause();
        playing_ = false;
    }

    auto pipeline = create_pipeline(video_path, use_hardware_decode);
    if (!pipeline) {
        if (was_playing) {
            playback_->play();
            playing_ = true;
        }
        return -1;
    }
    pipeline->decode_thread->set_pause_after_preroll(!was_playing);

    pipeline->file_id = next_file_id_++;
    int new_file_id = pipeline->file_id;
    configure_track_seek_callback(*pipeline);
    configure_track_error_callback(*pipeline);
    register_track_audio(*pipeline);
    if (!pipeline->demux_thread->start_thread()) {
        spdlog::error("Renderer::add_track: failed to start demux thread for {}", video_path);
        unregister_track_audio(new_file_id);
        pipeline->decode_thread->stop();
        pipeline->demux_thread->stop();
        if (was_playing) {
            playback_->play();
            playing_ = true;
        }
        return -1;
    }

    // Register with render sink
    render_sink_->set_track(slot, pipeline->track_buffer);
    render_sink_->set_track_offset(slot, 0);

    // Update duration cache
    cached_duration_us_ = std::max(cached_duration_us_,
        pipeline->demux_thread->stats().duration_us);

    // Commit: install the pipeline
    if (frame_presenter_) {
        frame_presenter_->reset_track(slot);
    }
    tracks_[slot] = std::move(pipeline);

    layout_controller_.append_track(layout_, new_file_id, slot);

    // Seek new track to current clock position so evaluate() can find matching frames.
    // Without this, the new track starts from PTS=0 and evaluate() discards all its
    // frames as "expired" when the clock is elsewhere, causing both panels to show
    // the same old video.
    int64_t current_pts = playback_->clock().current_pts_us();
    if (current_pts > 0) {
        auto& track = tracks_[slot];
        int64_t track_target = clamp_track_seek_target_us_locked(*track, current_pts);
        track->decode_thread->set_decode_paused(true);
        track->track_buffer->set_state(TrackState::Flushing);
        track->track_buffer->clear_frames();
        track->packet_queue->flush();
        if (track->audio_packet_queue) {
            track->audio_packet_queue->flush();
        }
        if (audio_coordinator_) {
            audio_coordinator_->set_track_decode_paused(track->file_id, true);
        }
        const auto seek_type = was_playing ? SeekType::Keyframe : SeekType::Exact;
        track->seek_controller->request_seek(track_target, seek_type);
        track->track_buffer->set_state(TrackState::Buffering);
        spdlog::info("Renderer::add_track: seeking slot={} to {:.3f}s (offset={:.3f}s, type={})",
                     slot,
                     track_target / 1e6,
                     track->offset_us / 1e6,
                     seek_type == SeekType::Exact ? "Exact" : "Keyframe");
    }

    // Force redraw, but keep already-presented frames from existing tracks so
    // they remain visible while the new track is still buffering/soft-decoding.
    preview_drawn_ = false;
    last_decision_.frames[slot] = std::nullopt;

    spdlog::info("Renderer::add_track: slot={} hw_decode={} path={}",
                 slot,
                 use_hardware_decode,
                 video_path);
    return slot;
}

void Renderer::remove_track(int file_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    int slot = find_slot_by_file_id(file_id);
    if (slot < 0) return;

    spdlog::info("Renderer::remove_track: file_id={}, slot={}", file_id, slot);

    // Pause playback
    bool was_playing = playing_.load();
    if (was_playing) {
        playback_->pause();
        playing_ = false;
    }

    tracks_.stop_slot(slot, [this](size_t stopped_slot, TrackPipeline& track) {
        unregister_track_audio(track.file_id);
        render_sink_->set_track(stopped_slot, nullptr);
        if (frame_presenter_) {
            frame_presenter_->reset_track(stopped_slot);
        }
    });

    // Compact: shift tracks_[slot+1..] down to fill the gap
    tracks_.compact_from(slot, [this](size_t from, size_t to, TrackPipeline& track) {
        if (frame_presenter_) {
            frame_presenter_->move_track(from, to);
        }
        render_sink_->set_track(to, track.track_buffer);
        render_sink_->set_track_offset(to, track.offset_us);
        render_sink_->set_track(from, nullptr);
    });

    // Compact last_decision_.frames the same way
    for (size_t i = slot; i < kMaxTracks - 1; ++i) {
        last_decision_.frames[i] = std::move(last_decision_.frames[i + 1]);
    }
    last_decision_.frames[kMaxTracks - 1] = std::nullopt;

    layout_controller_.remove_track(
        layout_, file_id, [this](int id) { return find_slot_by_file_id(id); });

    // Recalculate duration
    cached_duration_us_ = 0;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            cached_duration_us_ = std::max(cached_duration_us_,
                tracks_[i]->demux_thread->stats().duration_us);
        }
    }

    preview_drawn_ = false;

    // If still have tracks and was playing, resume
    if (was_playing && first_active_track() >= 0) {
        playback_->play();
        playing_ = true;
    }

    spdlog::info("Renderer::remove_track: file_id={}, slot={}, remaining={}", file_id, slot, track_count());
}

bool Renderer::has_track(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks)) return false;
    return tracks_[slot] != nullptr;
}

std::pair<int, int> Renderer::track_dimensions(int slot) const {
    if (slot < 0 || slot >= static_cast<int>(kMaxTracks) || !tracks_[slot]) {
        return {0, 0};
    }
    return {tracks_[slot]->video_width, tracks_[slot]->video_height};
}

std::vector<TrackInfo> Renderer::track_infos() const {
    std::vector<TrackInfo> infos;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            const auto* demux = tracks_[i]->demux_thread.get();
            const auto* decode = tracks_[i]->decode_thread.get();
            infos.push_back({
                tracks_[i]->file_id,
                static_cast<int>(i),
                tracks_[i]->file_path,
                tracks_[i]->video_width,
                tracks_[i]->video_height,
                demux ? demux->stats().duration_us : 0,
                demux ? demux->stats().start_time_us : 0,
                demux ? demux->stats().bit_rate : 0,
                demux ? demux->stats().format_name : std::string{},
                demux ? demux->stats().codec_name : std::string{},
                demux ? demux->stats().codec_long_name : std::string{},
                decode ? decode->decoder_name() : std::string{}
            });
        }
    }
    return infos;
}

std::vector<TrackPerfStats> Renderer::track_perf_stats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    std::vector<TrackPerfStats> result;
    auto now = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(now - stats_start_time_).count();

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) continue;
        const auto& track = tracks_[i];
        auto snap = track->decode_thread->perf_counters().snapshot();

        TrackPerfStats s;
        s.slot = static_cast<int>(i);
        s.file_id = track->file_id;
        s.buffer_count = track->track_buffer->total_count();
        s.buffer_capacity = track->track_buffer->preroll_target();
        s.buffer_state = track->track_buffer->state();
        if (last_decision_.frames[i].has_value()) {
            const auto& current_frame = last_decision_.frames[i].value();
            s.current_pts_us = current_frame.pts_us;
            s.current_dts_us = current_frame.dts_us;
        }

        // Average decode time
        if (snap.frames_decoded > 0) {
            s.avg_decode_ms = static_cast<double>(snap.total_decode_us) /
                              static_cast<double>(snap.frames_decoded) / 1000.0;
        }
        s.max_decode_ms = static_cast<double>(snap.max_decode_us) / 1000.0;

        // FPS: delta frames / delta time since last snapshot
        auto& baseline = perf_baselines_[i];
        uint64_t delta_frames = snap.frames_decoded - baseline.frames;
        if (elapsed_s > 0.5) {
            s.fps = static_cast<double>(delta_frames) / elapsed_s;
            baseline.frames = snap.frames_decoded;
        }

        result.push_back(s);
    }

    // Reset shared timer once after all tracks are processed
    if (elapsed_s > 0.5) {
        stats_start_time_ = now;
    }
    return result;
}

D3D11BackendMetrics Renderer::d3d_backend_metrics() const {
    D3D11BackendMetrics result;
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

RendererGpuMemoryStats Renderer::gpu_memory_stats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    std::lock_guard<std::recursive_mutex> device_lock(device_mutex_);
    RendererGpuMemoryStats result;

    const auto presenter_stats = frame_presenter_
        ? frame_presenter_->memory_stats()
        : D3D11FramePresenterMemoryStats{};
    result.presenter_texture_bytes = presenter_stats.total_estimated_bytes;
    result.total_estimated_bytes += result.presenter_texture_bytes;

    if (headless_output_) {
        const auto headless_stats = headless_output_->memory_stats();
        result.headless_output_bytes = headless_stats.estimated_bytes;
        result.headless_width = headless_stats.width;
        result.headless_height = headless_stats.height;
        result.headless_buffer_count = headless_stats.buffer_count;
        result.total_estimated_bytes += result.headless_output_bytes;
    }

    if (d3d_resources_) {
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (d3d_resources_->overlay_rect_capacity[i] > 0) {
                result.analysis_overlay_bytes +=
                    static_cast<uint64_t>(d3d_resources_->overlay_rect_capacity[i]) *
                    static_cast<uint64_t>(AnalysisOverlayRenderer::gpu_rect_size());
            }
            if (d3d_resources_->overlay_textures[i]) {
                D3D11_TEXTURE2D_DESC desc = {};
                d3d_resources_->overlay_textures[i]->GetDesc(&desc);
                result.analysis_overlay_bytes += estimate_d3d11_texture_bytes(desc);
                result.analysis_overlay_width =
                    std::max(result.analysis_overlay_width, static_cast<int>(desc.Width));
                result.analysis_overlay_height =
                    std::max(result.analysis_overlay_height, static_cast<int>(desc.Height));
            }
            if (d3d_resources_->overlay_mask_textures[i]) {
                D3D11_TEXTURE2D_DESC mask_desc = {};
                d3d_resources_->overlay_mask_textures[i]->GetDesc(&mask_desc);
                result.analysis_overlay_bytes += estimate_d3d11_texture_bytes(mask_desc);
                result.analysis_overlay_width =
                    std::max(result.analysis_overlay_width, static_cast<int>(mask_desc.Width));
                result.analysis_overlay_height =
                    std::max(result.analysis_overlay_height, static_cast<int>(mask_desc.Height));
            }
        }
        if (result.analysis_overlay_bytes > 0) {
            result.total_estimated_bytes += result.analysis_overlay_bytes;
        }
    }

    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) {
            continue;
        }

        TrackGpuMemoryStats track;
        track.slot = static_cast<int>(i);
        track.file_id = tracks_[i]->file_id;
        if (tracks_[i]->track_buffer) {
            track.buffer_count = tracks_[i]->track_buffer->total_count();
            track.buffer_capacity = tracks_[i]->track_buffer->max_count();
            track.track_buffer_cpu_bytes =
                tracks_[i]->track_buffer->estimated_cpu_bytes();
        }
        if (tracks_[i]->packet_queue) {
            track.packet_queue_bytes = tracks_[i]->packet_queue->estimated_bytes();
        }
        if (tracks_[i]->decode_thread) {
            const auto decode_stats = tracks_[i]->decode_thread->memory_stats();
            track.hardware_enabled = decode_stats.hardware_enabled;
            track.hardware_download_to_cpu = decode_stats.hardware_download_to_cpu;
            track.hw_format = decode_stats.hw_format;
            track.sw_format = decode_stats.sw_format;
            track.hw_width = decode_stats.hw_width;
            track.hw_height = decode_stats.hw_height;
            track.hw_initial_pool_size = decode_stats.hw_initial_pool_size;
            track.extra_hw_frames = decode_stats.extra_hw_frames;
            track.decoder_frame_bytes = decode_stats.estimated_hw_frame_bytes;
            track.decoder_pool_bytes = decode_stats.estimated_hw_pool_bytes;
            track.exact_seek_snapshot_bytes = decode_stats.snapshot_pool.estimated_bytes;
            track.exact_seek_candidate_cpu_bytes =
                decode_stats.exact_seek_candidate_cpu_bytes;
            track.exact_seek_stable_cpu_bytes =
                decode_stats.exact_seek_stable_cpu_bytes;
            track.exact_seek_reorder_count = decode_stats.exact_seek_reorder_count;
            track.exact_seek_pending_count = decode_stats.exact_seek_pending_count;
            track.exact_seek_stable_frame_count =
                decode_stats.exact_seek_stable_frame_count;
        }
        if (i < presenter_stats.slots.size()) {
            track.presenter_copy_texture_bytes =
                presenter_stats.slots[i].render_nv12_copy_texture_bytes;
        }
        track.total_cpu_frame_bytes =
            track.track_buffer_cpu_bytes +
            track.exact_seek_candidate_cpu_bytes +
            track.exact_seek_stable_cpu_bytes;
        result.decoder_pool_bytes += track.decoder_pool_bytes;
        result.exact_seek_snapshot_bytes += track.exact_seek_snapshot_bytes;
        result.track_buffer_cpu_bytes += track.track_buffer_cpu_bytes;
        result.packet_queue_bytes += track.packet_queue_bytes;
        result.exact_seek_candidate_cpu_bytes += track.exact_seek_candidate_cpu_bytes;
        result.exact_seek_stable_cpu_bytes += track.exact_seek_stable_cpu_bytes;
        result.cpu_frame_bytes += track.total_cpu_frame_bytes;
        result.total_estimated_bytes +=
            track.decoder_pool_bytes + track.exact_seek_snapshot_bytes;
        result.tracks.push_back(track);
    }

    return result;
}

bool Renderer::d3d_device_lost() const {
    return device_state_.load(std::memory_order_acquire) != RendererDeviceState::Ready ||
           (d3d_device_ && d3d_device_->device_lost());
}

long Renderer::d3d_device_removed_reason() const {
    return d3d_device_ ? static_cast<long>(d3d_device_->device_removed_reason())
                       : static_cast<long>(S_OK);
}

RendererDeviceState Renderer::device_state() const {
    return device_state_.load(std::memory_order_acquire);
}

} // namespace vr
