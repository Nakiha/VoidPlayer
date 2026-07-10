#include "renderer/renderer_internal.h"

namespace vr {

bool Renderer::Impl::initialize(const RendererConfig& config) {
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

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (initialized_.load() || loop_driver_.running() || loop_driver_.thread_joinable()) {
        spdlog::warn("Renderer: initialize called while already initialized or running");
        return false;
    }

    auto fail = [this]() {
        release_resources_locked();
        return false;
    };

    surface_state_.configure(config);
    layout_state_.reset_revisions();
    shutting_down_.store(false, std::memory_order_release);
    device_state_.store(RendererDeviceState::Ready, std::memory_order_release);
    presentation_metrics_.reset();
    timeline_.start_session_if_needed();

    PresentationBackendConfig backend_config;
    backend_config.hwnd = surface_state_.hwnd();
    backend_config.adapter = config.backend.adapter;
    backend_config.output = config.backend.output;
    backend_config.width = surface_state_.width();
    backend_config.height = surface_state_.height();
    backend_config.max_track_slots = config.backend.max_track_slots;
    backend_config.headless = surface_state_.headless();
    backend_config.output_target = config.backend.output_target;
    backend_config.sdr_white_level_nits =
        config.backend.sdr_white_level_nits;
    const auto* backend_provider = config.backend.provider
        ? config.backend.provider
        : default_presentation_backend_provider();
    auto backend = backend_provider &&
                   backend_provider->supports(surface_state_.backend_kind())
        ? backend_provider->create(surface_state_.backend_kind())
        : nullptr;
    if (!backend) {
        spdlog::error("Renderer: unsupported presentation backend {}",
                      render_backend_kind_name(surface_state_.backend_kind()));
        return fail();
    }
    if (!backend->initialize(backend_config)) {
        return fail();
    }
    presentation_.set_backend(std::move(backend));

    int next_initial_file_id = config.initial_file_id;
    const InitialTrackOpenHooks initial_track_hooks{
        [this](const std::string& path, bool use_hardware_decode) {
            return track_controller_.create_pipeline(
                path,
                use_hardware_decode,
                surface_state_.backend_kind(),
                presentation_.native_render_device(),
                &presentation_.device_mutex());
        },
        [this, &next_initial_file_id]() {
            const int file_id = next_initial_file_id++;
            track_controller_.set_next_file_id(
                std::max(track_controller_.next_file_id(), file_id + 1));
            return file_id;
        },
        TrackPipelineStartHooks{
            [this](TrackPipeline& track) { configure_track_seek_callback(track); },
            [this](TrackPipeline& track) { configure_track_error_callback(track); },
            [this](TrackPipeline& track) { register_track_audio(track); },
            [this](int file_id) { unregister_track_audio(file_id); },
        },
    };
    track_controller_.open_initial_tracks(
        config.video_paths, config.use_hardware_decode, initial_track_hooks, "Renderer");
    track_controller_.assign_missing_generations();

    if (!track_controller_.has_active_tracks()) {
        spdlog::error("Renderer: no valid tracks");
        return fail();
    }

    layout_state_.reset();
    for (const auto& track : track_controller_.layout_track_references()) {
        layout_state_.append_track(track.file_id, track.slot);
    }

    // Setup render sink
    render_sink_ = std::make_unique<RenderSink>(timeline_.playback().clock());
    track_controller_.bind_to_render_sink(*render_sink_);

    // Cache duration (immutable until tracks are added/removed)
    track_controller_.recompute_cached_duration();

    initialized_ = true;

    track_controller_.reset_perf_baseline(std::chrono::steady_clock::now());

    // Start render loop immediately (paused mode).
    // Decodes and displays first frame, fills buffers, but does not advance playback.
    loop_driver_.set_running(true);
    try {
        loop_driver_.start_thread(&Renderer::Impl::render_loop, this);
    } catch (const std::exception& e) {
        spdlog::error("Renderer: failed to start render thread: {}", e.what());
        loop_driver_.set_running(false);
        initialized_ = false;
        return fail();
    }

    spdlog::info("Renderer: initialized with {} tracks", track_controller_.count());
    return true;
}

void Renderer::Impl::shutdown() {
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
        loop_driver_.set_running(false);
        timeline_.set_playing(false);
    }

    spdlog::info("Renderer: shutdown begin");

    if (loop_driver_.thread_joinable()) {
        spdlog::info("Renderer: waiting for render thread join");
        loop_driver_.join_thread();
        spdlog::info("Renderer: render thread joined");
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        release_resources_locked();
    }

    spdlog::info("Renderer: shutdown complete");
}

bool Renderer::Impl::has_resources_locked() const {
    return track_controller_.has_active_tracks() ||
           presentation_.has_backend() ||
           render_sink_ ||
           initialized_.load() ||
           loop_driver_.running() ||
           loop_driver_.thread_joinable();
}

void Renderer::Impl::release_resources_locked() {
    loop_driver_.set_running(false);
    timeline_.set_playing(false);
    clear_event_callback();

    // Clear cached frames that may hold hw decode surface references.
    // Must happen before decode_thread->stop() frees hw_device_ctx,
    // otherwise hw_frame_ref cleanup will access a freed device context.
    present_history_.reset();
    loop_driver_.reset_presentation_scheduler();

    track_controller_.stop_all([this](size_t, TrackPipeline& track) {
        unregister_track_audio(track.file_id);
    });

    render_sink_.reset();
    timeline_.stop_session_if_started();
    presentation_.shutdown_backend();

    surface_state_.reset();
    track_controller_.set_cached_duration_us(0);
    track_controller_.reset_ids();
    layout_state_.reset();
    layout_state_.reset_revisions();
    if (analysis_overlay_renderer_) {
        analysis_overlay_renderer_->reset();
    }
    loop_driver_.reset_preview_state();
    loop_driver_.reset_preroll_state();
    if (timeline_.seek()) {
        timeline_.seek()->reset();
    }
    timeline_.reset_loop_range();
    loop_driver_.clear_pending_resize();
    loop_driver_.reset_timing();
    track_controller_.reset_perf_baseline();
    initialized_ = false;
    device_state_.store(RendererDeviceState::Ready, std::memory_order_release);
}

} // namespace vr
