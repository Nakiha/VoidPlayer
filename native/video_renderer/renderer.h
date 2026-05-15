#pragma once
#include "playback/playback_controller.h"
#include "media/demux_thread.h"
#include "video_renderer/decode/decode_thread.h"
#include "video_renderer/decode/frame_converter.h"
#include "media/packet_queue.h"
#include "video_renderer/buffer/track_buffer.h"
#include "media/seek_controller.h"
#include "video_renderer/sync/render_sink.h"
#include "video_renderer/track/track_pipeline.h"
#include "video_renderer/capture/frame_capture_service.h"
#include "video_renderer/layout/layout_controller.h"
#include "video_renderer/layout/layout_state.h"
#include "video_renderer/render/render_loop_controller.h"
#include "video_renderer/render/renderer_draw_snapshot.h"
#include "video_renderer/render/renderer_device_state.h"
#include "video_renderer/seek/seek_coordinator.h"
#include "video_renderer/render/shader_constants.h"
#include "video_renderer/track/track_gpu_memory_stats.h"
#include "video_renderer/track/track_info.h"
#include "video_renderer/track/track_perf_baseline.h"
#include "video_renderer/track/track_perf_stats.h"
#include "video_renderer/track/track_pipeline_factory.h"
#include "common/logging.h"
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>  // IWYU pragma: keep
#include <functional>
#include <cstdint>

namespace vr {

class D3D11Device;
class D3D11FramePresenter;
class D3D11HeadlessOutput;
class D3D11RenderBackend;
struct D3D11RenderResources;
class ShaderManager;
class TextureManager;
class AudioCoordinator;
class SeekCoordinator;
class AnalysisOverlayRenderer;

struct RendererEvent {
    enum class Type {
        SeekPreviewPresented,
        TrackError,
    };

    Type type = Type::SeekPreviewPresented;
    int64_t request_id = -1;
    int track_file_id = -1;
    int64_t pts_us = -1;
    int64_t dts_us = kNoTimestampUs;
    int64_t target_pts_us = -1;
    int error_code = 0;
};

using RendererEventCallback = std::function<void(const RendererEvent&)>;

struct D3D11BackendMetrics {
    uint64_t render_wait_us = 0;
    uint64_t render_wait_count = 0;
    uint64_t frame_copy_us = 0;
    uint64_t frame_copy_count = 0;
    uint64_t present_publish_us = 0;
    uint64_t present_publish_count = 0;
    uint64_t shared_texture_resize_count = 0;
    uint64_t device_lost_count = 0;
    uint64_t texture_sharing_failure_count = 0;
};

struct RendererGpuMemoryStats {
    uint64_t total_estimated_bytes = 0;
    uint64_t decoder_pool_bytes = 0;
    uint64_t exact_seek_snapshot_bytes = 0;
    uint64_t presenter_texture_bytes = 0;
    uint64_t headless_output_bytes = 0;
    uint64_t analysis_overlay_bytes = 0;
    uint64_t cpu_frame_bytes = 0;
    uint64_t track_buffer_cpu_bytes = 0;
    uint64_t packet_queue_bytes = 0;
    uint64_t exact_seek_candidate_cpu_bytes = 0;
    uint64_t exact_seek_stable_cpu_bytes = 0;
    int headless_width = 0;
    int headless_height = 0;
    int headless_buffer_count = 0;
    int analysis_overlay_width = 0;
    int analysis_overlay_height = 0;
    std::vector<TrackGpuMemoryStats> tracks;
};

enum class RendererBackendType {
    D3D11 = 1,
};

/// Platform-specific renderer interop values.
/// D3D11 uses `adapter` as the Flutter Windows DXGI adapter pointer. Future
/// backends can add their own opaque handles without changing the
/// cross-platform RendererConfig fields.
struct RendererBackendInterop {
    RendererBackendType type = RendererBackendType::D3D11;
    void* adapter = nullptr;
};

struct RendererConfig {
    std::vector<std::string> video_paths;
    void* hwnd = nullptr;
    int width = 1920;
    int height = 1080;
    bool use_hardware_decode = true;

    /// Headless mode: render to offscreen texture instead of swap chain.
    bool headless = false;

    /// Native backend interop for headless mode.
    RendererBackendInterop backend;

    /// Logging configuration. Applied during initialize().
    /// Can also be set independently via configure_logging() before init.
    LogConfig log_config;
};

enum class SharedTextureHandleType {
    None = 0,
    D3D11SharedHandle = 1,
};

struct SharedTextureSnapshot {
    SharedTextureHandleType type = SharedTextureHandleType::None;
    void* texture = nullptr;  ///< AddRef'd backend texture; caller must Release().
    void* handle = nullptr;
    int width = 0;
    int height = 0;
    int buffer_index = -1;
    uint64_t buffer_generation = 0;
};

class Renderer {
public:
    Renderer();
    explicit Renderer(PlaybackController& playback);
    ~Renderer();

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
    D3D11BackendMetrics d3d_backend_metrics() const;
    RendererGpuMemoryStats gpu_memory_stats() const;

    bool d3d_device_lost() const;
    long d3d_device_removed_reason() const;
    RendererDeviceState device_state() const;

    /// Set per-track sync offset in microseconds.
    /// Positive = delayed start (blank lead-in), negative = early start (skip beginning).
    void set_track_offset(int file_id, int64_t offset_us);

    // -- Layout control --

    /// Atomically apply layout state and trigger redraw if paused.
    void apply_layout(const LayoutState& state);

    /// Set the viewport fill color used outside video bounds.
    void set_background_color(float r, float g, float b, float a);

    /// Get a snapshot of the current layout state (thread-safe).
    LayoutState layout() const;

    // -- Headless mode: texture sharing --

    /// Set callback invoked after each frame is drawn in headless mode.
    void set_frame_callback(std::function<void()> cb);

    /// Set callback invoked for low-frequency renderer/player events.
    void set_event_callback(RendererEventCallback cb);

    /// Get actual texture dimensions (may lag behind resize request).
    int texture_width() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return target_width_;
    }
    int texture_height() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return target_height_;
    }

    /// Acquire the current headless texture and shared handle as one snapshot.
    /// The returned texture is AddRef'd and must be released by the caller.
    bool acquire_shared_texture(SharedTextureSnapshot& snapshot) const;
    void release_shared_texture(int buffer_index, uint64_t buffer_generation) const;

    /// Resize the offscreen shared texture (headless mode only).
    /// Stores pending dimensions; render loop applies at controlled rate.
    void resize(int width, int height);

    /// Capture the currently published headless frame as packed BGRA bytes.
    bool capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height);

    // Native test seams for lifecycle states that are otherwise only reachable
    // through render-thread or host-callback timing.
    bool has_event_callback_for_test() const;
    void enter_terminal_render_loop_error_for_test(const char* reason);

private:
    void render_loop() noexcept;
    void render_loop_body();
    bool draw_frame(const RendererDrawSnapshot& snapshot);
    void draw_paused_frame(const char* reason);
    RendererDrawSnapshot build_draw_snapshot_locked(const PresentDecision& decision) const;
    void update_track_geometry_from_decision_locked(const PresentDecision& decision);
    void seek_internal(std::unique_lock<std::mutex>& state_lock,
                       int64_t target_pts_us,
                       SeekType type,
                       bool allow_deferred = true,
                       bool force_recreate_paused_hevc = false);
    void apply_playback_decode_state_locked(bool playback_active);
    void set_decode_paused_for_all_tracks(bool paused);
    void configure_track_seek_callback(TrackPipeline& track);
    void configure_track_error_callback(TrackPipeline& track);
    void register_track_audio(TrackPipeline& track);
    void unregister_track_audio(int file_id);
    bool should_defer_paused_hevc_seek_locked(const RendererSeekClockGatePlan& gate);
    bool apply_deferred_paused_hevc_seek_locked(std::unique_lock<std::mutex>& state_lock);
    bool apply_loop_range_locked(std::unique_lock<std::mutex>& state_lock);
    void mark_paused_hevc_seek_preview_drawn_locked();
    bool has_hevc_hw_track_locked() const;
    void emit_event(const RendererEvent& event);
    void emit_seek_preview_presented_events(const PresentDecision& decision);
    void clear_event_callback();

    /// Apply pending resize on the render thread.
    void do_resize(int width, int height);

    /// Draw frame, present/flush, set preview_drawn_.
    void present_frame(const PresentDecision& decision);

    /// Headless-only: select back buffer RTV, draw, swap, notify Flutter.
    /// Caller must hold device_mutex_; texture_mutex_ is held only while
    /// selecting and publishing the shared buffer. Callers must not already
    /// hold texture_mutex(); callbacks returned from this method run outside
    /// both locks.
    bool draw_headless_and_publish(const RendererDrawSnapshot& snapshot,
                                   const char* label,
                                   std::function<void()>& callback);

    /// Internal mutex for D3D11 headless texture access.
    std::mutex& texture_mutex() const;

    /// Lightweight layout-only redraw (no Flush) for responsive zoom/pan during playback.
    void redraw_layout();

    /// Issue GPU fence and spin-wait for completion without publishing buffers.
    void wait_gpu_idle(const char* label);

    /// Find the first active track slot (for clock reference).
    /// Returns -1 if no tracks are active.
    int first_active_track() const;

    /// Find the first empty slot. Returns -1 if all full.
    int find_empty_slot() const;

    /// Find the slot index for a given file_id. Returns -1 if not found.
    int find_slot_by_file_id(int file_id) const;

    /// Create a TrackPipeline for the given video path.
    /// Returns nullptr if pipeline init fails (demux/decode errors).
    std::unique_ptr<TrackPipeline> create_pipeline(
        const std::string& path,
        bool hw_decode = true,
        const SeekRequest* initial_seek = nullptr);

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

    /// Resolve the logical playback end across active tracks.
    /// Caller must hold state_mutex_.
    int64_t effective_duration_us_locked() const;

    /// Stop playback at EOF once all buffers are drained.
    /// Caller must hold state_mutex_.
    bool settle_eof_locked(int64_t max_presented_end_us);

    /// Enter the terminal device-lost state. Automatic recovery is not
    /// implemented yet, so this stops rendering and leaves teardown to shutdown.
    /// Caller must hold state_mutex_.
    void enter_terminal_device_lost_locked(const char* operation);
    /// Caller must hold state_mutex_.
    void enter_terminal_render_loop_error_locked(const char* reason);
    void reset_d3d_metrics();
    void assign_missing_track_generations_locked();
    D3D11Device* d3d_device() const;
    D3D11FramePresenter* frame_presenter() const;
    D3D11HeadlessOutput* headless_output() const;
    D3D11RenderResources* d3d_resources() const;

    std::unique_ptr<PlaybackController> owned_playback_;
    PlaybackController* playback_ = nullptr;
    bool playback_session_started_by_renderer_ = false;
    std::unique_ptr<AudioCoordinator> audio_coordinator_;
    std::unique_ptr<SeekCoordinator> seek_coordinator_;
    std::unique_ptr<AnalysisOverlayRenderer> analysis_overlay_renderer_;
    std::unique_ptr<D3D11RenderBackend> d3d_backend_;
    std::unique_ptr<RenderSink> render_sink_;
    FrameCaptureService frame_capture_;
    LayoutController layout_controller_;
    RenderLoopController render_loop_controller_;

    TrackPipelineFactory track_pipeline_factory_;
    TrackPipelineManager tracks_;

    std::thread render_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> shutting_down_{false};
    std::atomic<RendererDeviceState> device_state_{RendererDeviceState::Ready};
    struct D3D11BackendMetricCounters {
        std::atomic<uint64_t> render_wait_us{0};
        std::atomic<uint64_t> render_wait_count{0};
        std::atomic<uint64_t> frame_copy_us{0};
        std::atomic<uint64_t> frame_copy_count{0};
        std::atomic<uint64_t> present_publish_us{0};
        std::atomic<uint64_t> present_publish_count{0};
        std::atomic<uint64_t> shared_texture_resize_count{0};
        std::atomic<uint64_t> device_lost_count{0};
        std::atomic<uint64_t> texture_sharing_failure_count{0};
    };
    mutable D3D11BackendMetricCounters d3d_metrics_;

    // Renderer lock contract is documented in native/docs/THREADING_MODEL.md.
    // Allowed nesting: lifecycle_mutex_ -> state_mutex_ -> device_mutex_ ->
    // texture_mutex(). Callbacks must run outside these locks.
    mutable std::mutex lifecycle_mutex_;
    mutable std::mutex state_mutex_;
    mutable std::mutex event_callback_mutex_;
    RendererEventCallback event_callback_;
    float background_color_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    bool preview_drawn_ = false;
    bool was_buffering_ = false;
    LoopRangeState loop_range_;

    mutable TrackPerfBaselineTracker perf_baseline_tracker_;

    // Shared lock for D3D11 immediate context serialization.
    // Both the render thread and FFmpeg's D3D11VA decode threads must acquire
    // this lock before using the immediate context. Without it, concurrent
    // access causes driver-level deadlocks.
    mutable std::recursive_mutex device_mutex_;

    int target_width_ = 1920;
    int target_height_ = 1080;
    void* hwnd_ = nullptr;
    int64_t cached_duration_us_ = 0;

    // -- Layout state --
    LayoutState layout_;
    int next_file_id_ = 1;                         ///< Auto-incrementing file ID
    uint64_t next_track_generation_ = 1;

    // -- Cached last frame for redraws (zoom/pan while paused or at EOF) --
    PresentDecision last_decision_;
    int64_t pending_seek_event_request_id_ = -1;
    int64_t pending_seek_event_target_pts_us_ = -1;
    bool pending_seek_event_emitted_ = true;

    // -- Headless mode state --
    bool headless_ = false;

    mutable std::mutex texture_mutex_fallback_;

    // Resize debounce: store pending dimensions, render loop applies at controlled rate.
    std::atomic<int> pending_width_{0};
    std::atomic<int> pending_height_{0};
};

} // namespace vr
