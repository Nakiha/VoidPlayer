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
#include "video_renderer/logging.h"
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>  // IWYU pragma: keep
#include <wrl/client.h>

namespace vr {

/// Layout mode constants (match HLSL defines)
constexpr int LAYOUT_SIDE_BY_SIDE = 0;
constexpr int LAYOUT_SPLIT_SCREEN = 1;

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
    void resume();
    void seek(int64_t target_pts_us, SeekType type = SeekType::Keyframe);
    void set_speed(double speed);

    // Frame stepping (pause + advance/retreat)
    void step_forward();
    void step_backward();

    bool is_playing() const;
    bool is_initialized() const;
    int64_t current_pts_us() const;
    double current_speed() const;

    size_t track_count() const;
    int64_t duration_us() const;

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

    /// Get the DXGI shared handle for the offscreen texture.
    HANDLE shared_texture_handle() const { return shared_handle_; }

    /// Mutex for thread-safe access to shared texture.
    std::mutex& texture_mutex() { return texture_mutex_; }

private:
    void render_loop();
    void draw_frame(const PresentDecision& decision);
    void draw_paused_frame(const char* reason);
    void seek_internal(int64_t target_pts_us, SeekType type);
    int64_t compute_frame_duration_us() const;

    /// Lock device + texture mutexes, draw frame, present/flush, set preview_drawn_.
    void present_frame(const PresentDecision& decision);

    /// Lightweight layout-only redraw (no Flush) for responsive zoom/pan during playback.
    void redraw_layout();

    /// Check if any frame slot in a PresentDecision has a value.
    static bool has_any_frame(const PresentDecision& decision);

    Clock clock_;
    std::unique_ptr<D3D11Device> d3d_device_;
    std::unique_ptr<TextureManager> texture_mgr_;
    std::unique_ptr<ShaderManager> shader_mgr_;
    std::unique_ptr<RenderSink> render_sink_;
    CompiledShader compiled_shader_;

    struct TrackPipeline {
        std::string file_path;
        std::unique_ptr<PacketQueue> packet_queue;
        std::unique_ptr<TrackBuffer> track_buffer;
        std::unique_ptr<DemuxThread> demux_thread;
        std::unique_ptr<DecodeThread> decode_thread;
        std::unique_ptr<SeekController> seek_controller;

        // Cached video dimensions (immutable after init)
        int video_width = 0;
        int video_height = 0;
        float video_aspect = 16.0f / 9.0f;

        // Cached render resources (reused across frames to avoid per-frame allocation)
        Microsoft::WRL::ComPtr<ID3D11Texture2D> sw_texture;       // Pooled RGBA upload texture
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sw_srv;  // SRV for sw texture
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nv12_y_srv;  // Cached NV12 Y SRV
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> nv12_uv_srv; // Cached NV12 UV SRV
        void* last_nv12_tex = nullptr;  // Pointer to detect when hw texture changes
        int last_nv12_idx = -1;         // Array index to detect when slice changes
        float nv12_uv_scale_y = 1.0f;  // video_height / texture_height (alignment padding fix)
    };

    std::vector<std::unique_ptr<TrackPipeline>> tracks_;

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

    // -- Cached last frame for redraws (zoom/pan while paused or at EOF) --
    PresentDecision last_decision_;

    // -- Headless mode state --
    bool headless_ = false;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> shared_texture_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> shared_rtv_;
    HANDLE shared_handle_ = nullptr;
    std::mutex texture_mutex_;
    std::function<void()> frame_callback_;
};

} // namespace vr
