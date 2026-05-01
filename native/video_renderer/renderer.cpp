#include "video_renderer/renderer.h"
#include "embedded_shaders.h"
#include "video_renderer/d3d11/frame_presenter.h"
#include "video_renderer/d3d11/headless_output.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <algorithm>
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
static constexpr size_t kTrackForwardDepth = 4;
static constexpr size_t kTrackBackwardDepth = 1;

DecodeDeviceMode default_decode_device_mode(AVCodecID codec_id) {
    if (codec_id == AV_CODEC_ID_AV1 || codec_id == AV_CODEC_ID_VP9) {
        return DecodeDeviceMode::FfmpegOwnedHwDownloadDevice;
    }
    return DecodeDeviceMode::IndependentDevice;
}

Renderer::Renderer()
    : owned_playback_(std::make_unique<PlaybackController>())
    , playback_(owned_playback_.get()) {}

Renderer::Renderer(PlaybackController& playback)
    : playback_(&playback) {}

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::initialize(const RendererConfig& config) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);

    // Flutter plugin configures logging before initialize().
    // Skip empty config to avoid clearing all sinks.
    if (!config.log_config.file_path.empty() || config.log_config.level != spdlog::level::info) {
        configure_logging(config.log_config);
    }

    // Install crash handler if file path is set
    if (!config.log_config.file_path.empty()) {
        std::string crash_dir;
        auto last_sep = config.log_config.file_path.find_last_of("/\\");
        if (last_sep != std::string::npos) {
            crash_dir = config.log_config.file_path.substr(0, last_sep);
        }
        install_crash_handler(crash_dir);
    }

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (initialized_.load() || running_.load() || render_thread_.joinable()) {
        spdlog::warn("Renderer: initialize called while already initialized or running");
        return false;
    }

    if (config.video_paths.empty()) {
        spdlog::error("Renderer: no video paths provided");
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
    playback_session_started_by_renderer_ = false;
    if (!playback_->audio_engine()) {
        playback_->start_session();
        playback_session_started_by_renderer_ = true;
    }

    d3d_device_ = std::make_unique<D3D11Device>();
    if (config.headless) {
        if (!d3d_device_->initialize_headless(config.dxgi_adapter, target_width_, target_height_)) {
            spdlog::error("Renderer: failed to initialize D3D11 device (headless)");
            return fail();
        }
        headless_output_ = std::make_unique<D3D11HeadlessOutput>();
        if (!headless_output_->initialize(d3d_device_->device(), d3d_device_->context(),
                                          target_width_, target_height_)) {
            return fail();
        }
    } else {
        if (!d3d_device_->initialize(hwnd_, target_width_, target_height_)) {
            spdlog::error("Renderer: failed to initialize D3D11 device");
            return fail();
        }
    }

    texture_mgr_ = std::make_unique<TextureManager>(d3d_device_->device(), d3d_device_->context());
    frame_presenter_ = std::make_unique<D3D11FramePresenter>(texture_mgr_.get(), d3d_device_->context());
    shader_mgr_ = std::make_unique<ShaderManager>(d3d_device_->device());

    if (!shader_mgr_->compile_from_source(kMultitrackHlsl, "VSMain", "PSMain", compiled_shader_)) {
        spdlog::error("Renderer: failed to compile shaders");
        return fail();
    }

    // Create constant buffer for shader uniforms (must be 16-byte aligned)
    // Layout must match multitrack.hlsl cbuffer Constants
    if (!shader_mgr_->create_constant_buffer(d3d_device_->device(), 224, compiled_shader_)) {
        spdlog::error("Renderer: failed to create constant buffer");
        return fail();
    }

    // Create sampler state
    D3D11_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MinLOD = 0;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    HRESULT hr = d3d_device_->device()->CreateSamplerState(&sampler_desc, &sampler_state_);
    if (FAILED(hr) || !sampler_state_) {
        spdlog::error("Renderer: CreateSamplerState failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return fail();
    }

    // Create vertex buffer (fullscreen quad)
    struct Vertex { float x, y, u, v; };
    Vertex quad[] = {
        {-1, -1, 0, 1},  // bottom-left (UV flipped for D3D)
        {-1,  1, 0, 0},  // top-left
        { 1, -1, 1, 1},  // bottom-right
        { 1,  1, 1, 0},  // top-right
    };
    D3D11_BUFFER_DESC vb_desc = {};
    vb_desc.ByteWidth = sizeof(quad);
    vb_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA vb_data = {};
    vb_data.pSysMem = quad;
    hr = d3d_device_->device()->CreateBuffer(&vb_desc, &vb_data, &vertex_buffer_);
    if (FAILED(hr) || !vertex_buffer_) {
        spdlog::error("Renderer: CreateBuffer(vertex) failed: HRESULT {:#x}",
                      static_cast<unsigned long>(hr));
        return fail();
    }

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
        register_track_audio(*pipeline);
        tracks_[slot] = std::move(pipeline);
    }

    bool any_track = false;
    for (const auto& t : tracks_) { if (t) { any_track = true; break; } }
    if (!any_track) {
        spdlog::error("Renderer: no valid tracks");
        return fail();
    }

    // Initialize file_id_order_ and slot-based order
    {
        int pos = 0;
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (tracks_[i]) {
                file_id_order_[pos] = tracks_[i]->file_id;
                layout_.order[pos] = static_cast<int>(i);
                ++pos;
            }
        }
        for (int i = pos; i < 4; ++i) {
            file_id_order_[i] = -1;
            layout_.order[i] = 0;
        }
    }

    // Setup render sink
    render_sink_ = std::make_unique<RenderSink>(playback_->clock());
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            render_sink_->set_track(i, tracks_[i]->track_buffer.get());
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
                        texture_mgr_ ||
                        frame_presenter_ ||
                        headless_output_ ||
                        shader_mgr_ ||
                        render_sink_ ||
                        sampler_state_ ||
                        vertex_buffer_ ||
                        cached_rtv_ ||
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

    // Stop all tracks
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) {
            unregister_track_audio(tracks_[i]->file_id);
            spdlog::info("Renderer: stopping track[{}] decode ({})", i, tracks_[i]->file_path);
            tracks_[i]->decode_thread->stop();
            spdlog::info("Renderer: track[{}] decode stopped", i);
            spdlog::info("Renderer: stopping track[{}] demux ({})", i, tracks_[i]->file_path);
            tracks_[i]->demux_thread->stop();
            spdlog::info("Renderer: track[{}] demux stopped", i);
            tracks_[i].reset();
        }
    }

    render_sink_.reset();
    if (playback_ && playback_session_started_by_renderer_) {
        playback_->stop_session();
        playback_session_started_by_renderer_ = false;
    }
    shader_mgr_.reset();
    frame_presenter_.reset();
    texture_mgr_.reset();

    // ComPtr auto-releases — just reset
    cached_rtv_.Reset();
    headless_output_.reset();
    sampler_state_.Reset();
    vertex_buffer_.Reset();

    if (d3d_device_) {
        d3d_device_->shutdown();
        d3d_device_.reset();
    }

    hwnd_ = nullptr;
    headless_ = false;
    target_width_ = 1920;
    target_height_ = 1080;
    cached_duration_us_ = 0;
    next_file_id_ = 1;
    layout_ = LayoutState();
    for (int i = 0; i < 4; ++i) {
        file_id_order_[i] = -1;
    }
    preview_drawn_ = false;
    was_buffering_ = false;
    deferred_paused_hevc_seek_ = DeferredSeekRequest();
    paused_hevc_seek_in_flight_ = false;
    paused_hevc_initial_settle_done_ = false;
    paused_hevc_seek_settle_until_ = std::chrono::steady_clock::time_point{};
    loop_range_ = LoopRangeState();
    pending_width_.store(0);
    pending_height_.store(0);
    last_resize_time_ = std::chrono::steady_clock::time_point{};
    stats_start_time_ = std::chrono::steady_clock::time_point{};
    for (auto& bl : perf_baselines_) bl.frames = 0;
    initialized_ = false;
}

void Renderer::play() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!initialized_ || playing_) return;
    deferred_paused_hevc_seek_.pending = false;
    paused_hevc_seek_in_flight_ = false;
    paused_hevc_initial_settle_done_ = false;
    paused_hevc_seek_settle_until_ = std::chrono::steady_clock::time_point{};

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

void Renderer::seek(int64_t target_pts_us, SeekType type) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    seek_internal(target_pts_us, type);
}

void Renderer::set_loop_range(bool enabled, int64_t start_us, int64_t end_us) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!enabled || end_us <= start_us) {
        loop_range_ = LoopRangeState();
        spdlog::info("[Renderer] loop range disabled");
        return;
    }
    loop_range_.enabled = true;
    loop_range_.start_us = std::max<int64_t>(0, start_us);
    loop_range_.end_us = std::max(loop_range_.start_us, end_us);
    spdlog::info("[Renderer] loop range enabled: {:.3f}s -> {:.3f}s",
                 loop_range_.start_us / 1e6, loop_range_.end_us / 1e6);
}

void Renderer::set_audible_track(int file_id) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!playback_->audio_engine()) return;
    if (file_id >= 0 && find_slot_by_file_id(file_id) < 0) {
        file_id = -1;
    }
    playback_->audio_engine()->set_active_track(file_id);
}

int Renderer::audible_track() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return playback_->audio_engine() ? playback_->audio_engine()->active_track() : -1;
}

void Renderer::seek_internal(int64_t target_pts_us,
                             SeekType type,
                             bool allow_deferred,
                             bool force_recreate_paused_hevc) {
    // Caller must hold state_mutex_
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
        // Per-track target: subtract this track's sync offset
        int64_t track_target = std::max(target_pts_us - track->offset_us, int64_t(0));
        // Pause decoder FIRST to prevent stale packets from reaching the codec
        // (avoids HEVC "Could not find ref" warnings during seek transition)
        track->decode_thread->set_decode_paused(true);
        if (playback_->audio_engine()) {
            playback_->audio_engine()->set_track_decode_paused(track->file_id, true);
        }
        auto buf_count_before = track->track_buffer->total_count();
        auto pq_size_before = track->packet_queue->size();
        const auto buffer_state_before = track->track_buffer->state();
        track->track_buffer->set_state(TrackState::Flushing);
        track->track_buffer->clear_frames();
        if (frame_presenter_) {
            // Seek invalidates the decoder surface epoch; reopen shared NV12
            // resources when the new exact-seek frame arrives.
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
    if (playing_.load() || type != SeekType::Exact) {
        return false;
    }

    bool has_hevc_hw_track = false;
    for (const auto& track : tracks_) {
        if (!track) continue;
        if (track->decode_thread->is_hardware_decode_enabled() &&
            track->decode_thread->codec_id() == AV_CODEC_ID_HEVC) {
            has_hevc_hw_track = true;
            break;
        }
    }
    if (!has_hevc_hw_track) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!paused_hevc_seek_in_flight_ && now >= paused_hevc_seek_settle_until_) {
        paused_hevc_seek_in_flight_ = true;
        deferred_paused_hevc_seek_.pending = false;
        return false;
    }

    deferred_paused_hevc_seek_.pending = true;
    deferred_paused_hevc_seek_.target_pts_us = target_pts_us;
    deferred_paused_hevc_seek_.type = type;
    spdlog::info("[Renderer] Deferring paused HEVC HW seek to {:.3f}s (in_flight={}, settle_remaining_ms={})",
                 target_pts_us / 1e6,
                 paused_hevc_seek_in_flight_,
                 now < paused_hevc_seek_settle_until_
                     ? static_cast<long long>(
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               paused_hevc_seek_settle_until_ - now).count())
                     : 0LL);
    return true;
}

bool Renderer::apply_deferred_paused_hevc_seek_locked() {
    if (playing_.load() ||
        !deferred_paused_hevc_seek_.pending ||
        paused_hevc_seek_in_flight_ ||
        std::chrono::steady_clock::now() < paused_hevc_seek_settle_until_) {
        return false;
    }

    auto deferred = deferred_paused_hevc_seek_;
    deferred_paused_hevc_seek_.pending = false;
    paused_hevc_seek_in_flight_ = true;
    spdlog::info("[Renderer] Applying deferred paused HEVC HW seek to {:.3f}s",
                 deferred.target_pts_us / 1e6);
    seek_internal(deferred.target_pts_us, deferred.type, false, true);
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
    bool has_hevc_hw_track = false;
    for (const auto& track : tracks_) {
        if (!track) continue;
        if (track->decode_thread->is_hardware_decode_enabled() &&
            track->decode_thread->codec_id() == AV_CODEC_ID_HEVC) {
            has_hevc_hw_track = true;
            break;
        }
    }
    if (!has_hevc_hw_track) {
        return;
    }

    if (paused_hevc_seek_in_flight_) {
        paused_hevc_seek_in_flight_ = false;
        paused_hevc_seek_settle_until_ = std::chrono::steady_clock::now() + kPausedHevcSeekSettleDelay;
        spdlog::info("[Renderer] Paused HEVC HW seek preview ready, settle window {}ms",
                     static_cast<long long>(kPausedHevcSeekSettleDelay.count()));
        return;
    }

    if (!paused_hevc_initial_settle_done_) {
        paused_hevc_initial_settle_done_ = true;
        paused_hevc_seek_settle_until_ = std::chrono::steady_clock::now() + kPausedHevcSeekSettleDelay;
        spdlog::info("[Renderer] Initial paused HEVC HW preview ready, settle window {}ms",
                     static_cast<long long>(kPausedHevcSeekSettleDelay.count()));
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

    const float track_w = static_cast<float>(tracks_[track_idx]->video_width);
    const float track_h = static_cast<float>(tracks_[track_idx]->video_height);
    float track_density = 1.0f;
    if (track_w > 0.0f && track_h > 0.0f) {
        track_density = std::min(slot_w / track_w, slot_h / track_h);
    }
    const float track_scale = (track_density > 0.0f) ? ref_density / track_density : 1.0f;

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
                    playback_->clock().seek(frame->pts_us);
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
    if (headless_output_) {
        headless_output_->wait_gpu_idle(label);
    } else if (d3d_device_) {
        d3d_device_->context()->Flush();
    }
}

void Renderer::draw_headless_and_publish(const PresentDecision& decision, const char* label) {
    if (!headless_output_) {
        return;
    }
    cached_rtv_ = headless_output_->begin_frame_locked();
    draw_frame(decision);
    headless_output_->publish_frame_locked(label);
    preview_drawn_ = true;
}

void Renderer::present_frame(const PresentDecision& decision) {
    spdlog::debug("[present_frame] mode={}", layout_.mode);
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    std::lock_guard<std::mutex> tex_lock(texture_mutex());
    if (headless_) {
        draw_headless_and_publish(decision, "present_frame");
    } else {
        draw_frame(decision);
        d3d_device_->present(0);
        preview_drawn_ = true;
    }
}

void Renderer::redraw_layout() {
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    std::lock_guard<std::mutex> tex_lock(texture_mutex());
    if (headless_) {
        draw_headless_and_publish(last_decision_, "redraw_layout");
    } else {
        draw_frame(last_decision_);
        d3d_device_->context()->Flush();
        preview_drawn_ = true;
    }
}

bool Renderer::capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_ || !headless_output_) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    std::lock_guard<std::mutex> tex_lock(texture_mutex());
    return headless_output_->capture_front_buffer_locked(bgra, width, height);
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
    if (playback_->audio_engine()) {
        playback_->audio_engine()->set_all_decode_paused(paused);
    }
}

void Renderer::configure_track_seek_callback(TrackPipeline& track) {
    auto* dt = track.decode_thread.get();
    const int file_id = track.file_id;
    track.demux_thread->set_seek_callback(
        [this, dt, file_id](int64_t pts, SeekType type) {
            dt->notify_seek(pts, type);
            if (playback_->audio_engine()) {
                playback_->audio_engine()->notify_seek(file_id, pts, type);
            }
        });
}

void Renderer::register_track_audio(TrackPipeline& track) {
    if (!playback_->audio_engine() ||
        !track.audio_packet_queue ||
        !track.demux_thread ||
        track.file_id <= 0) {
        return;
    }
    const auto& stats = track.demux_thread->stats();
    if (stats.audio_stream_index < 0 || !stats.audio_codec_params) {
        return;
    }
    if (!playback_->audio_engine()->add_track(
            track.file_id,
            *track.audio_packet_queue,
            stats.audio_codec_params,
            stats.audio_time_base)) {
        spdlog::warn("[Renderer] Failed to start audio decoder for file_id={}", track.file_id);
    }
}

void Renderer::unregister_track_audio(int file_id) {
    if (playback_->audio_engine() && file_id > 0) {
        playback_->audio_engine()->remove_track(file_id);
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

int64_t Renderer::effective_duration_us_locked() const {
    int64_t duration_us = cached_duration_us_;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i] || !tracks_[i]->demux_thread) continue;
        const int64_t track_duration_us =
            tracks_[i]->demux_thread->stats().duration_us;
        if (track_duration_us <= 0) continue;
        duration_us = std::max(duration_us, track_duration_us + tracks_[i]->offset_us);
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
    if (duration_us > 0 &&
        std::llabs(duration_us - max_presented_end_us) <= eof_tolerance_us) {
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

ID3D11Texture2D* Renderer::shared_texture() const {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_output_) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(texture_mutex());
    return headless_output_->shared_texture_locked();
}

HANDLE Renderer::shared_texture_handle() const {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_output_) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(texture_mutex());
    return headless_output_->shared_texture_handle_locked();
}

bool Renderer::acquire_shared_texture(SharedTextureSnapshot& snapshot) const {
    snapshot = {};

    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_output_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(texture_mutex());
    ID3D11Texture2D* texture = headless_output_->shared_texture_locked();
    HANDLE handle = headless_output_->shared_texture_handle_locked();
    if (!texture || !handle) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    texture->AddRef();

    snapshot.texture = texture;
    snapshot.handle = handle;
    snapshot.width = static_cast<int>(desc.Width);
    snapshot.height = static_cast<int>(desc.Height);
    return true;
}

std::mutex& Renderer::texture_mutex() const {
    return headless_output_ ? headless_output_->texture_mutex() : texture_mutex_fallback_;
}

void Renderer::resize(int width, int height) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_ || !d3d_device_) return;
    if (width <= 0 || height <= 0) return;
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

    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        std::lock_guard<std::mutex> tex_lock(texture_mutex());
        if (!headless_output_ || !headless_output_->resize_locked(width, height)) {
            return;
        }

        target_width_ = width;
        target_height_ = height;

        draw_headless_and_publish(last_decision_, "resize");
    }
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
                            preserve_requested_clock = paused_hevc_seek_in_flight_;
                            mark_paused_hevc_seek_preview_drawn_locked();
                        }
                        // For paused HEVC HW exact seeks, keep the logical
                        // clock at the user's requested target. The decoded
                        // preview can land on the nearest displayable frame,
                        // but the timeline should not visually snap backward.
                        int ref = first_active_track();
                        if (!preserve_requested_clock &&
                            ref >= 0 &&
                            preview.frames[ref].has_value()) {
                            playback_->clock().seek(preview.frames[ref]->pts_us);
                        }
                        spdlog::info("[Renderer] Paused frame: pts={:.3f}s",
                                     ref >= 0 && preview.frames[ref].has_value()
                                     ? preview.frames[ref]->pts_us / 1e6 : -1.0);
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
            // Only carry over if the track is still within its effective window.
            for (size_t i = 0; i < kMaxTracks; ++i) {
                if (!decision.frames[i].has_value() &&
                    last_decision_.frames[i].has_value() && tracks_[i]) {
                    int64_t effective_pts = decision.current_pts_us - tracks_[i]->offset_us;
                    int64_t track_dur = tracks_[i]->demux_thread->stats().duration_us;
                    if (effective_pts >= 0 && effective_pts <= track_dur) {
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
                        last_decision_.frames[i]->duration_us);
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
    auto* ctx = d3d_device_->context();

    // Get or create cached render target view
    if (!cached_rtv_) {
        if (!headless_) {
            ID3D11Texture2D* back_buffer = nullptr;
            HRESULT hr = d3d_device_->swap_chain()->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                                  reinterpret_cast<void**>(&back_buffer));
            if (FAILED(hr)) {
                spdlog::error("[Renderer] Failed to get back buffer: HRESULT {:#x}", static_cast<unsigned long>(hr));
                return;
            }
            hr = d3d_device_->device()->CreateRenderTargetView(back_buffer, nullptr, &cached_rtv_);
            back_buffer->Release();
            if (FAILED(hr)) {
                spdlog::error("[Renderer] Failed to create RTV: HRESULT {:#x}", static_cast<unsigned long>(hr));
                return;
            }
        }
    }

    if (!cached_rtv_) {
        return;
    }

    float clear_color[4] = {};
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (int i = 0; i < 4; ++i) {
            clear_color[i] = background_color_[i];
        }
    }
    ctx->ClearRenderTargetView(cached_rtv_.Get(), clear_color);
    ctx->OMSetRenderTargets(1, cached_rtv_.GetAddressOf(), nullptr);

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
    ID3D11Buffer* vb = vertex_buffer_.Get();
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    if (compiled_shader_.layout) {
        ctx->IASetInputLayout(compiled_shader_.layout.Get());
    }

    // Set shaders
    ctx->VSSetShader(compiled_shader_.vs.Get(), nullptr, 0);
    ctx->PSSetShader(compiled_shader_.ps.Get(), nullptr, 0);

    ID3D11ShaderResourceView* srvs[4] = {};           // t0-t3: RGBA (sw) or full NV12 (hw)
    ID3D11ShaderResourceView* nv12_y_srvs[4] = {};    // t4-t7: NV12 Y plane
    ID3D11ShaderResourceView* nv12_uv_srvs[4] = {};   // t8-t11: NV12 UV plane
    std::array<D3D11PreparedFrame, kMaxTracks> prepared_frames;
    if (frame_presenter_) {
        for (size_t i = 0; i < kMaxTracks; ++i) {
            if (!decision.frames[i].has_value() || !decision.frames[i]->texture_handle) continue;
            if (!tracks_[i]) continue;

            const bool prepared_ok = frame_presenter_->prepare_frame(
                i,
                decision.frames[i].value(),
                target_width_,
                target_height_,
                [this](const char* label) { wait_gpu_idle(label); },
                prepared_frames[i]);
            if (!prepared_ok) {
                continue;
            }

            srvs[i] = prepared_frames[i].rgba_srv;
            nv12_y_srvs[i] = prepared_frames[i].nv12_y_srv;
            nv12_uv_srvs[i] = prepared_frames[i].nv12_uv_srv;
        }
    }

    // Update constant buffer
    // Layout must match HLSL cbuffer Constants in multitrack.hlsl
    if (compiled_shader_.constant_buffer) {
        struct Constants {
            int mode;              // offset 0
            int track_count;       // offset 4
            float split_pos;       // offset 8
            float zoom_ratio;      // offset 12
            float canvas_width;    // offset 16
            float canvas_height;   // offset 20
            float view_offset[2];  // offset 24
            int order[4];          // offset 32
            float video_aspect[4]; // offset 48
            int nv12_mask;         // offset 64
            float _pad1[3];        // offset 68
            float nv12_uv_scale_y[4]; // offset 80
            float track_scale[4];  // offset 96: per-track scale for uniform pixel density

            // Precomputed per-track display params (offset 112-207)
            float display_offset_x[4];     // offset 112
            float display_offset_y[4];     // offset 128
            float inv_display_size_x[4];   // offset 144
            float inv_display_size_y[4];   // offset 160
            float view_offset_uv_x[4];    // offset 176
            float view_offset_uv_y[4];    // offset 192
            float background_color[4];     // offset 208
        };
        static_assert(sizeof(Constants) == 224, "Constants must be 224 bytes");

        // Snapshot layout state atomically
        LayoutState snap;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            snap = layout_;
        }

        Constants cb = {};
        cb.mode = snap.mode;
        cb.split_pos = snap.split_pos;
        cb.zoom_ratio = snap.zoom_ratio;
        cb.canvas_width = static_cast<float>(target_width_);
        cb.canvas_height = static_cast<float>(target_height_);
        cb.view_offset[0] = snap.view_offset[0];
        cb.view_offset[1] = snap.view_offset[1];
        cb.nv12_mask = 0;
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
                cb.nv12_uv_scale_y[i] = 1.0f;
                continue;
            }
            ++active_count;
            cb.video_aspect[i] = tracks_[i]->video_aspect;
            if (decision.frames[i].has_value() && decision.frames[i]->is_nv12) {
                cb.nv12_mask |= (1 << static_cast<int>(i));
                cb.nv12_uv_scale_y[i] = frame_presenter_
                    ? frame_presenter_->nv12_uv_scale_y(i)
                    : 1.0f;
            } else {
                cb.nv12_uv_scale_y[i] = frame_presenter_
                    ? frame_presenter_->nv12_uv_scale_y(i)
                    : 1.0f;
            }
        }
        cb.track_count = active_count;

        // Compute per-track scale for uniform pixel density across all tracks.
        // Find the reference track (highest resolution) and scale other tracks
        // so all videos share the same pixel density (video pixel -> screen pixel ratio).
        {
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
                if (!tracks_[i]) {
                    cb.track_scale[i] = 1.0f;
                    continue;
                }
                float tw = static_cast<float>(tracks_[i]->video_width);
                float th = static_cast<float>(tracks_[i]->video_height);
                float density = 1.0f;
                if (tw > 0.0f && th > 0.0f) {
                    density = std::min(slot_w / tw, slot_h / th);
                }
                cb.track_scale[i] = (density > 0.0f) ? ref_density / density : 1.0f;
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
                if (video_aspect <= 0.0f) video_aspect = slot_aspect;

                // Aspect-fit scale
                float fit_scale = (video_aspect > slot_aspect)
                    ? slot_aspect / video_aspect : 1.0f;
                fit_scale *= cb.track_scale[i];

                // Apply zoom
                float display_scale = fit_scale * snap.zoom_ratio;

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
        ctx->UpdateSubresource(compiled_shader_.constant_buffer.Get(), 0, nullptr, &cb, 0, 0);
        ctx->PSSetConstantBuffers(0, 1, compiled_shader_.constant_buffer.GetAddressOf());
    }

    // Set sampler
    if (sampler_state_) {
        ID3D11SamplerState* sampler = sampler_state_.Get();
        ctx->PSSetSamplers(0, 1, &sampler);
    }

    // Bind SRVs: t0-t3 RGBA, t4-t7 NV12 Y, t8-t11 NV12 UV
    ctx->PSSetShaderResources(0, 4, srvs);
    ctx->PSSetShaderResources(4, 4, nv12_y_srvs);
    ctx->PSSetShaderResources(8, 4, nv12_uv_srvs);

    // Draw
    ctx->Draw(4, 0);

    // Unbind SRVs before releasing to avoid GPU resource-in-use issues
    ID3D11ShaderResourceView* null_srvs[4] = {};
    ctx->PSSetShaderResources(0, 4, null_srvs);
    ctx->PSSetShaderResources(4, 4, null_srvs);
    ctx->PSSetShaderResources(8, 4, null_srvs);

    // Temporary direct-texture SRVs are owned by prepared_frames until draw returns.
}

// -- Layout control --
void Renderer::apply_layout(const LayoutState& state) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    layout_.mode = state.mode;
    layout_.split_pos = std::clamp(state.split_pos, 0.0f, 1.0f);
    layout_.zoom_ratio = std::clamp(state.zoom_ratio, 1.0f, 50.0f);
    layout_.view_offset[0] = state.view_offset[0];
    layout_.view_offset[1] = state.view_offset[1];

    // Translate file_id order → slot order for the shader
    for (int i = 0; i < 4; ++i) {
        file_id_order_[i] = state.order[i];
        int slot = find_slot_by_file_id(state.order[i]);
        layout_.order[i] = (slot >= 0) ? slot : 0;
    }

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
    LayoutState result = layout_;
    // Return file_id order (not slot order) to Flutter
    for (int i = 0; i < 4; ++i) {
        result.order[i] = file_id_order_[i];
    }
    return result;
}

// -- Dynamic track management --

int Renderer::first_active_track() const {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i]) return static_cast<int>(i);
    }
    return -1;
}

int Renderer::find_slot_by_file_id(int file_id) const {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (tracks_[i] && tracks_[i]->file_id == file_id)
            return static_cast<int>(i);
    }
    return -1;
}

int Renderer::find_empty_slot() const {
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks_[i]) return static_cast<int>(i);
    }
    return -1;
}

std::unique_ptr<Renderer::TrackPipeline> Renderer::create_pipeline(const std::string& path,
                                                                      bool hw_decode,
                                                                      const SeekRequest* initial_seek) {
    auto pipeline = std::make_unique<TrackPipeline>();
    pipeline->file_path = path;
    pipeline->use_hardware_decode = hw_decode;
    pipeline->seek_controller = std::make_unique<SeekController>();
    if (initial_seek) {
        pipeline->seek_controller->request_seek(initial_seek->target_pts_us, initial_seek->type);
    }
    pipeline->packet_queue = std::make_unique<PacketQueue>(100);
    pipeline->audio_packet_queue = std::make_unique<PacketQueue>(100);
    pipeline->track_buffer = std::make_unique<TrackBuffer>(kTrackForwardDepth, kTrackBackwardDepth);
    spdlog::info("Renderer: track buffer depth forward={}, backward={}, max_cached={}",
                 kTrackForwardDepth,
                 kTrackBackwardDepth,
                 kTrackForwardDepth + 1);
    pipeline->demux_thread = std::make_unique<DemuxThread>(
        path, *pipeline->seek_controller);
    pipeline->demux_thread->add_output(DemuxStreamKind::Video, *pipeline->packet_queue);
    pipeline->demux_thread->add_optional_output(DemuxStreamKind::Audio, *pipeline->audio_packet_queue);

    if (!pipeline->demux_thread->start()) {
        spdlog::error("Renderer: failed to start demux for {}", path);
        return nullptr;
    }

    const auto& stats = pipeline->demux_thread->stats();
    if (stats.video_stream_index < 0) {
        spdlog::error("Renderer: no video stream found in {}", path);
        pipeline->demux_thread->stop();
        return nullptr;
    }

    pipeline->video_width = stats.width;
    pipeline->video_height = stats.height;
    if (stats.width > 0 && stats.height > 0) {
        float sar = (stats.sar_den > 0)
            ? static_cast<float>(stats.sar_num) / static_cast<float>(stats.sar_den)
            : 1.0f;
        pipeline->video_aspect = (static_cast<float>(stats.width) / static_cast<float>(stats.height)) * sar;
    }

    pipeline->decode_thread = std::make_unique<DecodeThread>(
        *pipeline->packet_queue, *pipeline->track_buffer,
        stats.codec_params, stats.time_base);

    if (!pipeline->decode_thread->is_valid()) {
        spdlog::error("Renderer: decode thread init failed for {}", path);
        pipeline->demux_thread->stop();
        return nullptr;
    }

    pipeline->demux_thread->set_seek_callback(
        [dt = pipeline->decode_thread.get()](int64_t pts, SeekType type) {
            dt->notify_seek(pts, type);
        });

    if (hw_decode) {
        // Use an explicit policy so stable codecs stay on an independent
        // decode device, while AV1/VP9 keep the FFmpeg-owned hwdownload path.
        pipeline->decode_thread->enable_hardware_decode(
            default_decode_device_mode(stats.codec_params->codec_id));
    }

    if (!pipeline->decode_thread->start()) {
        spdlog::error("Renderer: failed to start decode for {}", path);
        pipeline->demux_thread->stop();
        return nullptr;
    }

    return pipeline;
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
    current->decode_thread->stop();
    current->demux_thread->stop();
    render_sink_->set_track(slot, nullptr);
    if (frame_presenter_) {
        frame_presenter_->reset_track(slot);
    }
    current.reset();

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
    register_track_audio(*replacement);

    render_sink_->set_track(slot, replacement->track_buffer.get());
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
            if (playback_->audio_engine()) {
                playback_->audio_engine()->notify_seek(file_id, pts, seek_type);
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

int Renderer::add_track(const std::string& video_path) {
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

    auto pipeline = create_pipeline(video_path);
    if (!pipeline) {
        if (was_playing) {
            playback_->play();
            playing_ = true;
        }
        return -1;
    }
    pipeline->decode_thread->set_pause_after_preroll(!was_playing);

    // Register with render sink
    render_sink_->set_track(slot, pipeline->track_buffer.get());
    render_sink_->set_track_offset(slot, 0);

    // Update duration cache
    cached_duration_us_ = std::max(cached_duration_us_,
        pipeline->demux_thread->stats().duration_us);

    // Commit: install the pipeline
    if (frame_presenter_) {
        frame_presenter_->reset_track(slot);
    }
    tracks_[slot] = std::move(pipeline);
    tracks_[slot]->file_id = next_file_id_++;
    int new_file_id = tracks_[slot]->file_id;
    configure_track_seek_callback(*tracks_[slot]);
    register_track_audio(*tracks_[slot]);

    // Append new file_id to the order arrays
    for (int i = 0; i < 4; ++i) {
        if (file_id_order_[i] < 0) {
            file_id_order_[i] = new_file_id;
            layout_.order[i] = slot;
            break;
        }
    }

    // Seek new track to current clock position so evaluate() can find matching frames.
    // Without this, the new track starts from PTS=0 and evaluate() discards all its
    // frames as "expired" when the clock is elsewhere, causing both panels to show
    // the same old video.
    int64_t current_pts = playback_->clock().current_pts_us();
    if (current_pts > 0) {
        auto& track = tracks_[slot];
        int64_t track_target = std::max(current_pts - track->offset_us, int64_t(0));
        track->decode_thread->set_decode_paused(true);
        track->track_buffer->set_state(TrackState::Flushing);
        track->track_buffer->clear_frames();
        track->packet_queue->flush();
        if (track->audio_packet_queue) {
            track->audio_packet_queue->flush();
        }
        if (playback_->audio_engine()) {
            playback_->audio_engine()->set_track_decode_paused(track->file_id, true);
        }
        track->seek_controller->request_seek(track_target, SeekType::Keyframe);
        track->track_buffer->set_state(TrackState::Buffering);
        spdlog::info("Renderer::add_track: seeking slot={} to {:.3f}s (offset={:.3f}s)",
                     slot, track_target / 1e6, track->offset_us / 1e6);
    }

    // Force redraw, but keep already-presented frames from existing tracks so
    // they remain visible while the new track is still buffering/soft-decoding.
    preview_drawn_ = false;
    last_decision_.frames[slot] = std::nullopt;

    spdlog::info("Renderer::add_track: slot={} path={}", slot, video_path);
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

    // Stop the pipeline
    auto& track = tracks_[slot];
    unregister_track_audio(track->file_id);
    track->decode_thread->stop();
    track->demux_thread->stop();

    // Unregister from render sink
    render_sink_->set_track(slot, nullptr);

    // Release the pipeline
    if (frame_presenter_) {
        frame_presenter_->reset_track(slot);
    }
    track.reset();

    // Compact: shift tracks_[slot+1..] down to fill the gap
    for (size_t i = slot; i < kMaxTracks - 1; ++i) {
        if (!tracks_[i + 1]) break;  // No more tracks to compact
        tracks_[i] = std::move(tracks_[i + 1]);
        if (frame_presenter_) {
            frame_presenter_->move_track(i + 1, i);
        }
        // Update render sink mapping (track buffer + offset)
        render_sink_->set_track(i, tracks_[i]->track_buffer.get());
        render_sink_->set_track_offset(i, tracks_[i]->offset_us);
        render_sink_->set_track(i + 1, nullptr);
    }

    // Compact last_decision_.frames the same way
    for (size_t i = slot; i < kMaxTracks - 1; ++i) {
        last_decision_.frames[i] = std::move(last_decision_.frames[i + 1]);
    }
    last_decision_.frames[kMaxTracks - 1] = std::nullopt;

    // Compact file_id_order_: remove the deleted file_id, shift remaining down
    for (int i = 0; i < 4; ++i) {
        if (file_id_order_[i] == file_id) {
            for (int j = i; j < 3; ++j) file_id_order_[j] = file_id_order_[j + 1];
            file_id_order_[3] = -1;
            break;
        }
    }

    // Re-translate file_id order → slot order after compact
    for (int i = 0; i < 4; ++i) {
        int s = find_slot_by_file_id(file_id_order_[i]);
        layout_.order[i] = (s >= 0) ? s : 0;
    }

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
            infos.push_back({
                tracks_[i]->file_id,
                static_cast<int>(i),
                tracks_[i]->file_path,
                tracks_[i]->video_width,
                tracks_[i]->video_height,
                tracks_[i]->demux_thread ? tracks_[i]->demux_thread->stats().duration_us : 0
            });
        }
    }
    return infos;
}

std::vector<TrackPerfStats> Renderer::track_perf_stats() const {
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

} // namespace vr
