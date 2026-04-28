#pragma once
#include "video_renderer/clock.h"
#include "video_renderer/d3d11/device.h"
#include "video_renderer/d3d11/texture.h"
#include "video_renderer/d3d11/shader.h"
#include "video_renderer/decode/demux_thread.h"
#include "video_renderer/decode/decode_thread.h"
#include "video_renderer/decode/frame_converter.h"
#include "video_renderer/buffer/packet_queue.h"
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/sync/seek_controller.h"
#include "video_renderer/sync/render_sink.h"
#include "common/logging.h"
#include <vector>
#include <array>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>  // IWYU pragma: keep
#include <wrl/client.h>

namespace vr {

class D3D11FramePresenter;
class D3D11HeadlessOutput;

/// Layout mode constants (match HLSL defines)
constexpr int LAYOUT_SIDE_BY_SIDE = 0;
constexpr int LAYOUT_SPLIT_SCREEN = 1;

/// Track metadata returned to the UI layer.
struct TrackInfo {
    int file_id;       ///< Stable identifier (auto-incrementing, survives reorder)
    int slot;
    std::string file_path;
    int width;
    int height;
    int64_t duration_us = 0;  ///< Track duration in microseconds
};

/// Per-track performance stats snapshot.
struct TrackPerfStats {
    int slot = -1;
    int file_id = 0;
    double fps = 0.0;
    double avg_decode_ms = 0.0;
    double max_decode_ms = 0.0;
    size_t buffer_count = 0;
    size_t buffer_capacity = 0;
    TrackState buffer_state = TrackState::Empty;
};

/// Layout state — all visual layout parameters in one struct.
/// Updated atomically via Renderer::apply_layout().
struct LayoutState {
    int mode = LAYOUT_SIDE_BY_SIDE;  // 0=SIDE_BY_SIDE, 1=SPLIT_SCREEN
    float split_pos = 0.5f;          // Split divider position (0.0-1.0)
    float zoom_ratio = 1.0f;         // 1.0=fit, >1.0=zoom in
    float view_offset[2] = {0.0f, 0.0f};  // Pan offset in pixel coordinates
    int order[4] = {0, 1, 2, 3};    // Track display order mapping
};

struct RendererConfig {
    std::vector<std::string> video_paths;
    void* hwnd = nullptr;
    int width = 1920;
    int height = 1080;
    bool use_hardware_decode = true;

    /// Headless mode: render to offscreen texture instead of swap chain.
    bool headless = false;

    /// DXGI adapter for headless mode (must match Flutter's adapter).
    IDXGIAdapter* dxgi_adapter = nullptr;

    /// Logging configuration. Applied during initialize().
    /// Can also be set independently via configure_logging() before init.
    LogConfig log_config;
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool initialize(const RendererConfig& config);
    void shutdown();

    void play();
    void pause();
    void seek(int64_t target_pts_us, SeekType type = SeekType::Keyframe);
    void set_speed(double speed);
    void set_loop_range(bool enabled, int64_t start_us, int64_t end_us);

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
    int add_track(const std::string& video_path);

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

    /// Set per-track sync offset in microseconds.
    /// Positive = delayed start (blank lead-in), negative = early start (skip beginning).
    void set_track_offset(int file_id, int64_t offset_us);

    // -- Layout control --

    /// Atomically apply layout state and trigger redraw if paused.
    void apply_layout(const LayoutState& state);

    /// Get a snapshot of the current layout state (thread-safe).
    LayoutState layout() const;

    // -- Headless mode: texture sharing --

    /// Set callback invoked after each frame is drawn in headless mode.
    void set_frame_callback(std::function<void()> cb);

    /// Get the shared offscreen texture (headless mode only).
    ID3D11Texture2D* shared_texture() const;

    /// Get actual texture dimensions (may lag behind resize request).
    int texture_width() const { return target_width_; }
    int texture_height() const { return target_height_; }

    /// Get the DXGI shared handle for the offscreen texture.
    HANDLE shared_texture_handle() const;

    /// Mutex for thread-safe access to shared texture.
    std::mutex& texture_mutex() const;

    /// Resize the offscreen shared texture (headless mode only).
    /// Stores pending dimensions; render loop applies at controlled rate.
    void resize(int width, int height);

    /// Capture the currently published headless frame as packed BGRA bytes.
    bool capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height);

private:
    struct TrackPipeline {
        int file_id = 0;              ///< Stable identifier assigned by add_track()
        std::string file_path;
        int64_t offset_us = 0;        ///< Per-track sync offset in microseconds
        std::unique_ptr<PacketQueue> packet_queue;
        std::unique_ptr<TrackBuffer> track_buffer;
        std::unique_ptr<DemuxThread> demux_thread;
        std::unique_ptr<DecodeThread> decode_thread;
        std::unique_ptr<SeekController> seek_controller;
        bool use_hardware_decode = true;
        bool recreated_for_paused_hevc_seek = false;

        // Cached video dimensions (immutable after init)
        int video_width = 0;
        int video_height = 0;
        float video_aspect = 16.0f / 9.0f;

    };

    void render_loop();
    void draw_frame(const PresentDecision& decision);
    void draw_paused_frame(const char* reason);
    bool build_step_forward_decision_locked(PresentDecision& decision) const;
    void discard_step_forward_consumed_frames_locked(const PresentDecision& decision);
    std::pair<float, float> display_pixel_size_for_layout_locked(
        int width, int height, const LayoutState& layout) const;
    void seek_internal(int64_t target_pts_us,
                       SeekType type,
                       bool allow_deferred = true,
                       bool force_recreate_paused_hevc = false);
    int64_t compute_frame_duration_us() const;
    void set_decode_paused_for_all_tracks(bool paused);
    bool should_defer_paused_hevc_seek_locked(int64_t target_pts_us, SeekType type);
    bool apply_deferred_paused_hevc_seek_locked();
    bool apply_loop_range_locked();
    void mark_paused_hevc_seek_preview_drawn_locked();

    /// Apply pending resize on the render thread.
    void do_resize(int width, int height);

    /// Lock device + texture mutexes, draw frame, present/flush, set preview_drawn_.
    void present_frame(const PresentDecision& decision);

    /// Headless-only: select back buffer RTV, draw, swap, notify Flutter.
    /// Caller must hold both device_mutex_ and texture_mutex_.
    void draw_headless_and_publish(const PresentDecision& decision, const char* label);

    /// Lightweight layout-only redraw (no Flush) for responsive zoom/pan during playback.
    void redraw_layout();

    /// Issue GPU fence and spin-wait for completion without publishing buffers.
    void wait_gpu_idle(const char* label);

    /// Check if any frame slot in a PresentDecision has a value.
    static bool has_any_frame(const PresentDecision& decision);

    /// Find the first active track slot (for clock reference).
    /// Returns -1 if no tracks are active.
    int first_active_track() const;

    /// Find the first empty slot. Returns -1 if all full.
    int find_empty_slot() const;

    /// Find the slot index for a given file_id. Returns -1 if not found.
    int find_slot_by_file_id(int file_id) const;

    /// Create a TrackPipeline for the given video path.
    /// Returns nullptr if pipeline init fails (demux/decode errors).
    std::unique_ptr<TrackPipeline> create_pipeline(const std::string& path,
                                                     bool hw_decode = true,
                                                     const SeekRequest* initial_seek = nullptr);

    /// Recreate a track pipeline so seek starts with a fresh demux/decode epoch.
    bool recreate_pipeline_for_seek(size_t slot, int64_t target_pts_us, SeekType type);

    /// Recreate only the decode thread/hardware decoder while keeping demux alive.
    bool recreate_decode_thread_for_seek(size_t slot, int64_t target_pts_us, SeekType type);

    Clock clock_;
    std::unique_ptr<D3D11Device> d3d_device_;
    std::unique_ptr<TextureManager> texture_mgr_;
    std::unique_ptr<D3D11FramePresenter> frame_presenter_;
    std::unique_ptr<D3D11HeadlessOutput> headless_output_;
    std::unique_ptr<ShaderManager> shader_mgr_;
    std::unique_ptr<RenderSink> render_sink_;
    CompiledShader compiled_shader_;

    std::array<std::unique_ptr<TrackPipeline>, kMaxTracks> tracks_;

    // Render resources — ComPtr for automatic COM lifecycle management
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertex_buffer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_state_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> cached_rtv_;

    std::thread render_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> playing_{false};
    mutable std::mutex state_mutex_;
    bool preview_drawn_ = false;
    bool was_buffering_ = false;
    struct DeferredSeekRequest {
        bool pending = false;
        int64_t target_pts_us = 0;
        SeekType type = SeekType::Keyframe;
    };
    DeferredSeekRequest deferred_paused_hevc_seek_;
    bool paused_hevc_seek_in_flight_ = false;
    bool paused_hevc_initial_settle_done_ = false;
    std::chrono::steady_clock::time_point paused_hevc_seek_settle_until_{};
    struct LoopRangeState {
        bool enabled = false;
        int64_t start_us = 0;
        int64_t end_us = 0;
    };
    LoopRangeState loop_range_;

    // -- Perf stats baseline for FPS calculation --
    mutable std::chrono::steady_clock::time_point stats_start_time_;
    struct PerfBaseline { uint64_t frames = 0; };
    mutable std::array<PerfBaseline, kMaxTracks> perf_baselines_;

    // Shared mutex for D3D11 immediate context serialization.
    // Both the render thread and FFmpeg's D3D11VA decode threads must acquire
    // this lock before using the immediate context. Without it, concurrent
    // access causes driver-level deadlocks.
    std::recursive_mutex device_mutex_;

    int target_width_ = 1920;
    int target_height_ = 1080;
    void* hwnd_ = nullptr;
    int64_t cached_duration_us_ = 0;

    // -- Layout state --
    LayoutState layout_;
    int next_file_id_ = 1;                         ///< Auto-incrementing file ID
    int file_id_order_[4] = {-1, -1, -1, -1};      ///< file_id order from Flutter

    // -- Cached last frame for redraws (zoom/pan while paused or at EOF) --
    PresentDecision last_decision_;

    // -- Headless mode state --
    bool headless_ = false;

    mutable std::mutex texture_mutex_fallback_;

    // Resize debounce: store pending dimensions, render loop applies at controlled rate.
    std::atomic<int> pending_width_{0};
    std::atomic<int> pending_height_{0};
    std::chrono::steady_clock::time_point last_resize_time_{};
};

} // namespace vr
