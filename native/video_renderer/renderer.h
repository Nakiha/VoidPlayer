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
#include "video_renderer/sync/render_sink.h"
#include "video_renderer/sync/seek_controller.h"
#include "video_renderer/logging.h"
#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>  // IWYU pragma: keep
#include <wrl/client.h>

namespace vr {

struct RendererConfig {
    std::vector<std::string> video_paths;
    void* hwnd = nullptr;
    int width = 1920;
    int height = 1080;
    bool use_hardware_decode = true;

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
    void seek(int64_t target_pts_us);
    void set_speed(double speed);

    bool is_playing() const;
    bool is_initialized() const;
    int64_t current_pts_us() const;
    double current_speed() const;

    size_t track_count() const;
    int64_t duration_us() const;

private:
    void render_loop();
    void draw_frame(const PresentDecision& decision);

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
        SeekController seek_controller;

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

    // Render resources
    ID3D11Buffer* vertex_buffer_ = nullptr;
    ID3D11SamplerState* sampler_state_ = nullptr;
    ID3D11RenderTargetView* cached_rtv_ = nullptr;

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
};

} // namespace vr
