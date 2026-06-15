#pragma once

#include "windows/d3d11/shared_fp16_ring.h"
#include "windows/d3d11/shared_source_cache_ring.h"
#include "windows/presentation/windows_dcomp_composite.h"
#include "windows/player/native_player.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dcomp.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class WindowsNativeCompositor {
public:
    enum class OutputTarget {
        SDR,
        ScRGB,
    };

    enum class Phase {
        Inactive,
        Preparing,
        Active,
        FallbackRestoring,
    };

    struct Diagnostics {
        std::string phase = "inactive";
        std::string fallback_reason = "none";
        std::string source_cache_last_error = "none";
        std::string output_target = "sdr";
        std::string desired_output_target = "sdr";
        std::string transition_state = "stable";
        std::string transition_reason = "initial";
        std::string swap_chain_format = "B8G8R8A8_UNORM";
        std::string color_space = "RGB_FULL_G22_NONE_P709";
        uint64_t state_serial = 0;
        uint64_t ack_serial = 0;
        uint64_t transition_serial = 0;
        uint64_t output_generation = 0;
        uint64_t hdr_promotion_count = 0;
        uint64_t hdr_demotion_count = 0;
        uint64_t target_fallback_count = 0;
        uint64_t flutter_generation = 0;
        uint64_t video_generation = 0;
        uint64_t composite_count = 0;
        uint64_t present_count = 0;
        uint64_t drop_count = 0;
        uint64_t failure_count = 0;
        uint64_t resize_count = 0;
        uint64_t diagnostic_capture_count = 0;
        uint64_t flutter_alpha_average_x1000 = 0;
        uint64_t flutter_transparent_pixels_x1000 = 0;
        uint64_t final_max_rgb_x1000 = 0;
        uint64_t final_pixels_over_1 = 0;
        uint64_t source_cache_consumed_generation = 0;
        uint64_t source_cache_fallback_count = 0;
        uint64_t source_projection_update_count = 0;
        uint64_t overlay_generation = 0;
        uint64_t overlay_fill_rect_count = 0;
        uint64_t overlay_line_rect_count = 0;
        uint64_t overlay_motion_line_count = 0;
        double source_cache_hz = 0.0;
        double source_projection_hz = 0.0;
        uint32_t swap_chain_width = 0;
        uint32_t swap_chain_height = 0;
        bool engine_export_available = false;
        bool swap_chain_active = false;
        bool color_space_supported = false;
        bool sdr_tone_map_active = true;
        bool source_projection_enabled = false;
        bool source_cache_active = false;
    };

    using StateCallback = std::function<void(Phase, uint64_t, const std::string&)>;
    using SourceProjection = vr::WindowsSourceProjection;

    WindowsNativeCompositor();
    ~WindowsNativeCompositor();

    bool Start(HWND hwnd,
               void* flutter_view,
               const std::shared_ptr<vr::NativePlayer>& player,
               IDXGIAdapter* adapter,
               double sdr_white_level_nits,
               OutputTarget output_target,
               StateCallback callback);
    void Stop(const char* reason = "shutdown");
    void SetViewportRect(double left, double top, double right, double bottom);
    void SetViewportBackgroundColor(uint32_t argb);
    void SetSourceProjection(const SourceProjection& projection);
    void ClearSourceProjection(const std::string& reason);
    void SetSourceCacheError(const std::string& error);
    void NotifySourceCachePublished();
    void RequestOutputTarget(OutputTarget target,
                             double sdr_white_level_nits,
                             uint64_t display_generation,
                             const std::string& reason);
    void AcknowledgeFlutterState(uint64_t serial, bool transparent_viewport);
    void ForceFallbackForTesting(const std::string& reason);
    void RequestDiagnosticCapture();
    Diagnostics diagnostics() const;

private:
    struct SwapChainResources {
        Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        OutputTarget target = OutputTarget::SDR;
        uint32_t width = 0;
        uint32_t height = 0;
        bool color_space_supported = false;
    };

    struct FlutterSurface {
        size_t struct_size = sizeof(FlutterSurface);
        HANDLE shared_texture_handle = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        int alpha_mode = 0;
        uint64_t ring_generation = 0;
        uint64_t frame_generation = 0;
        uint32_t slot = 0;
        uint64_t consumer_acquire_key = 1;
        uint64_t producer_release_key = 0;
        uint64_t lease_id = 0;
    };

    using SetExportModeFn = bool (*)(void*, int);
    using PublishedCallback = void (*)(void*, uint64_t, void*);
    using SetPublishedCallbackFn = void (*)(void*, PublishedCallback, void*);
    using AcquireFlutterSurfaceFn = bool (*)(void*, FlutterSurface*);
    using ReleaseFlutterSurfaceFn = bool (*)(void*, uint64_t);

    struct EngineApi {
        SetExportModeFn set_mode = nullptr;
        SetPublishedCallbackFn set_callback = nullptr;
        AcquireFlutterSurfaceFn acquire = nullptr;
        ReleaseFlutterSurfaceFn release = nullptr;
        bool available() const {
            return set_mode && set_callback && acquire && release;
        }
    };

    static void OnFlutterSurfacePublished(
        void* view, uint64_t generation, void* user_data);
    bool LoadEngineApi();
    bool InitializeDeviceAndComposition(IDXGIAdapter* adapter);
    bool CreateSwapChainCandidate(uint32_t width,
                                  uint32_t height,
                                  OutputTarget target,
                                  SwapChainResources& resources);
    bool EnsureSwapChain(uint32_t width, uint32_t height);
    bool ActivatePendingSwapChain();
    bool CreatePipeline();
    bool CaptureDiagnostics(ID3D11Texture2D* back_buffer,
                            ID3D11Texture2D* flutter_texture);
    bool DrawOverlay(
        const std::shared_ptr<const vr::AnalysisOverlayPrimitivePackage>& overlay,
        const SourceProjection& projection,
        const D3D11_TEXTURE2D_DESC& back_desc);
    void ReleaseHeldInputs(const std::shared_ptr<vr::NativePlayer>& player);
    void ThreadMain();
    bool CompositeLatest();
    void SignalWork();
    void EnterFallback(const std::string& reason);
    void PublishState(Phase phase, const std::string& reason);
    static const char* PhaseName(Phase phase);
    static const char* OutputTargetName(OutputTarget target);
    static const char* OutputFormatName(OutputTarget target);
    static const char* OutputColorSpaceName(OutputTarget target);

    HWND hwnd_ = nullptr;
    void* flutter_view_ = nullptr;
    std::weak_ptr<vr::NativePlayer> player_;
    std::atomic<double> sdr_white_scale_{1.0};
    uint64_t locked_display_generation_ = 0;
    EngineApi engine_api_;
    StateCallback state_callback_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    SwapChainResources current_swap_chain_;
    SwapChainResources pending_swap_chain_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> video_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> flutter_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> premultiplied_blend_state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> overlay_vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> overlay_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> overlay_input_layout_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> overlay_blend_state_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;

    bool held_video_valid_ = false;
    vr::SharedFp16TextureSnapshot held_video_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> held_video_texture_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> held_video_mutex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> held_video_srv_;

    bool held_sdr_video_valid_ = false;
    vr::SharedTextureSnapshot held_sdr_video_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> held_sdr_video_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> held_sdr_video_srv_;

    bool held_flutter_valid_ = false;
    FlutterSurface held_flutter_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> held_flutter_texture_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> held_flutter_mutex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> held_flutter_srv_;

    bool held_source_valid_ = false;
    vr::SharedSourceCacheBundleSnapshot held_source_;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 4>
        held_source_textures_;
    std::array<Microsoft::WRL::ComPtr<IDXGIKeyedMutex>, 4>
        held_source_mutexes_;
    std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 4>
        held_source_srvs_;
    std::array<bool, 4> held_source_present_{};
    std::array<int, 4> held_source_transfer_{};

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stop_ = false;
    bool work_pending_ = false;
    bool diagnostic_capture_pending_ = true;
    bool terminal_inactive_ = false;
    bool fallback_finish_pending_ = false;
    OutputTarget desired_output_target_ = OutputTarget::SDR;
    uint64_t transition_min_video_generation_ = 0;
    uint64_t transition_min_source_generation_ = 0;
    Phase phase_ = Phase::Inactive;
    uint64_t state_serial_ = 0;
    uint64_t ack_serial_ = 0;
    double viewport_[4] = {0.0, 0.0, 1.0, 1.0};
    float viewport_background_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    SourceProjection source_projection_;
    std::string source_cache_error_ = "none";
    std::chrono::steady_clock::time_point rate_start_time_{};
    uint64_t source_cache_publish_count_ = 0;
    bool source_cache_base_lease_wait_logged_ = false;
    bool source_cache_bundle_acquire_logged_ = false;
    bool source_cache_consumed_logged_ = false;
    Diagnostics diagnostics_;
};
