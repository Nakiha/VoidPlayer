#include "renderer/renderer_internal.h"

namespace vr {

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
    layout_intent_revision_.store(0, std::memory_order_relaxed);
    layout_revision_ = 0;
    last_presented_layout_revision_ = 0;
    clear_pending_layout_intent();
    shutting_down_.store(false, std::memory_order_release);
    device_state_.store(RendererDeviceState::Ready, std::memory_order_release);
    reset_presentation_backend_metrics();
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
    presentation_scheduler_.reset();

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
    layout_intent_revision_.store(0, std::memory_order_relaxed);
    layout_revision_ = 0;
    last_presented_layout_revision_ = 0;
    clear_pending_layout_intent();
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

} // namespace vr
