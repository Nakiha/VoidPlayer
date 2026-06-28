#pragma once
#include "audio/audio_output.h"
#include "playback/playback_controller.h"
#include "media/demux_thread.h"
#include "renderer/decode/decode_thread.h"
#include "renderer/decode/frame_converter.h"
#include "media/packet_queue.h"
#include "renderer/buffer/track_buffer.h"
#include "media/seek_controller.h"
#include "renderer/sync/render_sink.h"
#include "renderer/track/track_pipeline.h"
#include "renderer/layout/layout_state.h"
#include "renderer/layout/renderer_layout_state.h"
#include "renderer/events/renderer_event_bus.h"
#include "renderer/metrics/presentation_metrics_store.h"
#include "renderer/playback/renderer_timeline_controller.h"
#include "renderer/render/renderer_loop_driver.h"
#include "renderer/render/renderer_draw_snapshot.h"
#include "renderer/render/renderer_device_state.h"
#include "renderer/render/renderer_present_history.h"
#include "renderer/render/renderer_present_command.h"
#include "renderer/render/renderer_presentation_controller.h"
#include "renderer/render/renderer_render_loop_command.h"
#include "renderer/render/renderer_surface_state.h"
#include "renderer/render/presentation_backend.h"
#include "renderer/seek/seek_coordinator.h"
#include "renderer/render/shader_constants.h"
#include "renderer/renderer_config.h"
#include "renderer/track/track_gpu_memory_stats.h"
#include "renderer/track/track_info.h"
#include "renderer/track/track_perf_baseline.h"
#include "renderer/track/track_perf_stats.h"
#include "renderer/track/track_pipeline_factory.h"
#include "renderer/track/renderer_track_controller.h"
#include "renderer/renderer.h"
#include "common/logging.h"
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>  // IWYU pragma: keep
#include <functional>
#include <chrono>
#include <cstdint>
#include <optional>

namespace vr {

class ShaderManager;
class TextureManager;
class AudioCoordinator;
class SeekCoordinator;
class AnalysisOverlayRenderer;
struct SharedFp16TextureSnapshot;
struct SourceCacheTrackDescriptor;
struct SharedSourceCacheBundleSnapshot;
struct WindowsSourceProjection;

class Renderer::Impl {
public:
    Impl();
    explicit Impl(PlaybackController& playback);
    ~Impl();

    bool initialize(const RendererConfig& config);
    void shutdown();

    void play();
    void pause();
    void seek(int64_t target_pts_us,
              SeekType type = SeekType::Keyframe,
              int64_t request_id = -1);
    void set_speed(double speed);
    void set_loop_range(bool enabled, int64_t start_us, int64_t end_us);
    void set_audible_track(int file_id);
    int audible_track() const;
    bool has_audio() const;
    int audio_sample_rate() const;
    int audio_channels() const;
    AudioOutputStats audio_output_stats() const;

    // Frame stepping (pause + advance/retreat)
    void step_forward();
    void step_backward();

    bool is_playing() const;
    bool is_initialized() const;
    int64_t current_pts_us() const;
    double current_speed() const;

    size_t track_count() const;
    int64_t duration_us() const;

    // -- Dynamic track management --

    /// Add a video track at the first empty slot.
    /// Returns the slot index (0-3), or -1 if all slots are full or init fails.
    int add_track(const std::string& video_path,
                  bool use_hardware_decode = true);
    int add_track_with_file_id(const std::string& video_path,
                               int file_id,
                               bool use_hardware_decode = true);

    /// Remove a track by file_id. Stops its pipeline, compacts slots.
    void remove_track(int file_id);

    /// Query whether a slot is occupied.
    bool has_track(int slot) const;

    /// Get track dimensions for a slot. Returns {0,0} if empty.
    std::pair<int, int> track_dimensions(int slot) const;

    /// Get metadata for all active tracks.
    std::vector<TrackInfo> track_infos() const;

    /// Get per-track performance stats snapshot (thread-safe).
    std::vector<TrackPerfStats> track_perf_stats() const;
    RendererPresentedAnchorDiagnostics presented_anchor_diagnostics() const;
    PresentationBackendMetrics presentation_backend_metrics() const;
    PresentationBackendMetrics d3d_backend_metrics() const;
    PresentationBackendStats presentation_backend_stats() const;
    PresentationBackendDiagnostics presentation_backend_diagnostics() const;
    std::string presentation_backend_last_error() const;
    bool copy_last_presentation_frame_info(PresentationBackendFrameInfo* out) const;
    RendererGpuMemoryStats gpu_memory_stats() const;

    bool d3d_device_lost() const;
    long d3d_device_removed_reason() const;
    bool recover_presentation_device_loss(const char* reason, long removed_reason);
    RendererDeviceState device_state() const;

    /// Set per-track sync offset in microseconds.
    /// Positive = delayed start (blank lead-in), negative = early start (skip beginning).
    void set_track_offset(int file_id, int64_t offset_us);
    int64_t track_offset_us(int file_id) const;

    // -- Layout control --

    /// Atomically apply layout state and trigger redraw if paused.
    void apply_layout(const LayoutState& state);

    /// Set the viewport fill color used outside video bounds.
    void set_background_color(float r, float g, float b, float a);

    /// Get a snapshot of the current layout state (thread-safe).
    LayoutState layout() const;

    // -- Headless mode: texture sharing --

    /// Set callback invoked after each frame is drawn in headless mode.
    void set_frame_callback(RendererFrameCallback cb);
    void set_frame_failure_callback(std::function<void(const char*)> cb);

    /// Set callback invoked for low-frequency renderer/player events.
    void set_event_callback(RendererEventCallback cb);

    /// Get actual texture dimensions (may lag behind resize request).
    int texture_width() const;
    int texture_height() const;

    /// Acquire the current headless texture and shared handle as one snapshot.
    /// The returned texture is AddRef'd and must be released by the caller.
    bool acquire_shared_texture(SharedTextureSnapshot& snapshot) const;
    void release_shared_texture(int buffer_index, uint64_t buffer_generation) const;
    void* native_render_device() const;
    void* native_render_command_queue() const;
    bool acquire_shared_fp16_texture(SharedFp16TextureSnapshot& snapshot) const;
    void release_shared_fp16_texture(int buffer_index, uint64_t ring_generation) const;
    void set_shared_fp16_frame_callback(std::function<void()> cb);
    bool update_external_flutter_surface(
        const PresentationExternalD3D12Surface& surface);
    void clear_external_flutter_surface();
    bool draw_current_frame_to_external_d3d12_target(
        const PresentationExternalD3D12RenderTarget& target,
        const char* reason);
    bool configure_source_cache(
        const std::vector<SourceCacheTrackDescriptor>& descriptors);
    void clear_source_cache(const char* reason);
    bool update_source_projection(const WindowsSourceProjection& projection);
    void clear_source_projection();
    bool acquire_source_cache_bundle(
        SharedSourceCacheBundleSnapshot& snapshot) const;
    void release_source_cache_bundle(
        int buffer_index, uint64_t ring_generation) const;
    void set_source_cache_frame_callback(std::function<void()> cb);
    bool prewarm_presentation_target(int width, int height);

    /// Resize the offscreen shared texture (headless mode only).
    /// Stores pending dimensions; render loop applies at controlled rate.
    void resize(int width, int height);
    bool update_headless_output(void* output,
                                int width,
                                int height,
                                int max_track_slots);
    bool install_headless_output(void* output,
                                 int width,
                                 int height,
                                 int max_track_slots);
    bool install_headless_output_ring(const void* const* pixel_buffers,
                                      size_t pixel_buffer_count,
                                      void* displayed_pixel_buffer,
                                      void* protected_pixel_buffer,
                                      int width,
                                      int height,
                                      int max_track_slots);
    void mark_headless_output_displayed(void* pixel_buffer);
    void protect_headless_output(void* pixel_buffer);
    void release_headless_output(void* pixel_buffer);
    void clear_headless_output();

    /// Request an immediate redraw of the currently presentable frame.
    /// Returns false when the renderer cannot issue a refresh command.
    bool request_frame_refresh(const char* reason);
    bool update_presentation_sdr_white_level(double nits);
    bool draw_current_frame_sources(PresentationBackend& backend,
                                    PresentationSourceFrameTarget* targets,
                                    size_t target_count,
                                    std::string* error);
    std::shared_ptr<const AnalysisOverlayPrimitivePackage> current_overlay_primitives(
        std::string* error);

    /// Capture the currently published headless frame as packed BGRA bytes.
    bool capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height);
    bool capture_front_buffer_region(int x,
                                     int y,
                                     int width,
                                     int height,
                                     std::vector<uint8_t>& bgra,
                                     int& region_width,
                                     int& region_height);

    // Native test seams for lifecycle states that are otherwise only reachable
    // through render-thread or host-callback timing.
    bool has_event_callback_for_test() const;
    void enter_terminal_render_loop_error_for_test(const char* reason);

private:
    class SeekCommandProcessor {
    public:
        static void seek(Impl& renderer,
                         std::unique_lock<std::mutex>& state_lock,
                         int64_t target_pts_us,
                         SeekType type,
                         bool allow_deferred = true,
                         bool force_recreate_paused_hevc = false);
    };

    void render_loop() noexcept;
    void apply_playback_decode_state_locked(bool playback_active);
    void set_decode_paused_for_all_tracks(bool paused);
    void configure_track_seek_callback(TrackPipeline& track);
    void configure_track_error_callback(TrackPipeline& track);
    void register_track_audio(TrackPipeline& track);
    void unregister_track_audio(int file_id);
    bool apply_deferred_paused_hevc_seek_locked(std::unique_lock<std::mutex>& state_lock);
    bool apply_loop_range_locked(std::unique_lock<std::mutex>& state_lock);
    void mark_paused_hevc_seek_preview_drawn_locked();
    bool has_hevc_hw_track_locked() const;
    void emit_event(const RendererEvent& event);
    void emit_seek_preview_presented_events(const PresentDecision& decision);
    void emit_playback_clock_event(bool force);
    void clear_event_callback();
    void apply_layout_locked(const LayoutState& state, uint64_t revision);
    bool consume_pending_layout_locked();
    void clear_pending_layout_intent();
    bool should_present_frame_consume_pending_layout() const;
    void note_viewport_compositor_activity();
    bool should_suppress_playback_present_for_viewport_compositor() const;
    RendererPresentCommandContext present_command_context();

    /// Apply pending resize on the render thread.
    void do_resize(int width, int height);

    /// Build draw hooks for the optional analysis overlay.
    RendererPresentationOverlayHooks presentation_overlay_hooks();

    /// Recreate a track pipeline so seek starts with a fresh demux/decode epoch.
    bool recreate_pipeline_for_seek(std::unique_lock<std::mutex>& state_lock,
                                    size_t slot,
                                    int64_t target_pts_us,
                                    SeekType type);

    /// Release all owned renderer resources after the render thread has stopped.
    /// Caller must hold state_mutex_.
    void release_resources_locked();

    /// Return whether shutdown has native state or resources to release.
    /// Caller must hold state_mutex_.
    bool has_resources_locked() const;

    /// Stop playback at EOF once all buffers are drained.
    /// Caller must hold state_mutex_.
    bool settle_eof_locked(int64_t max_presented_end_us);

    /// Enter the terminal device-lost state after recovery has failed.
    /// Caller must hold state_mutex_.
    void enter_terminal_device_lost_locked(const char* operation);
    /// Caller must hold state_mutex_.
    bool recover_or_enter_terminal_device_lost_locked(const char* operation);
    /// Caller must hold state_mutex_.
    void enter_terminal_render_loop_error_locked(const char* reason);
    int add_track_internal(const std::string& video_path,
                           bool use_hardware_decode,
                           int requested_file_id);
    RendererTimelineController timeline_;
    std::unique_ptr<AnalysisOverlayRenderer> analysis_overlay_renderer_;
    RendererPresentationController presentation_;
    std::unique_ptr<RenderSink> render_sink_;
    RendererLayoutState layout_state_;
    RendererLoopDriver loop_driver_;
    RendererSurfaceState surface_state_;

    mutable RendererTrackController track_controller_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<RendererDeviceState> device_state_{RendererDeviceState::Ready};
    mutable PresentationMetricsStore presentation_metrics_;

    // Renderer lock contract is documented in native/docs/THREADING_MODEL.md.
    // Allowed nesting: lifecycle_mutex_ -> state_mutex_ -> device_mutex_ ->
    // backend texture locks. Callbacks must run outside these locks.
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex state_mutex_;
    RendererEventBus event_bus_;
    RendererPresentHistory present_history_;
    PresentDecision external_d3d12_visible_decision_;
    std::chrono::steady_clock::time_point last_playback_clock_event_time_{};

};

} // namespace vr
