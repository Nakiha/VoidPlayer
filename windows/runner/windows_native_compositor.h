#pragma once

#include "windows/d3d11/shared_fp16_ring.h"
#include "windows/player/native_player.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <dcomp.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class WindowsNativeCompositor {
public:
    enum class Phase {
        Inactive,
        Preparing,
        Active,
        FallbackRestoring,
    };

    struct Diagnostics {
        std::string phase = "inactive";
        std::string fallback_reason = "none";
        uint64_t state_serial = 0;
        uint64_t ack_serial = 0;
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
        uint32_t swap_chain_width = 0;
        uint32_t swap_chain_height = 0;
        bool engine_export_available = false;
        bool swap_chain_active = false;
    };

    using StateCallback = std::function<void(Phase, uint64_t, const std::string&)>;

    WindowsNativeCompositor();
    ~WindowsNativeCompositor();

    bool Start(HWND hwnd,
               void* flutter_view,
               const std::shared_ptr<vr::NativePlayer>& player,
               IDXGIAdapter* adapter,
               double sdr_white_level_nits,
               StateCallback callback);
    void Stop(const char* reason = "shutdown");
    void SetViewportRect(double left, double top, double right, double bottom);
    void AcknowledgeFlutterState(uint64_t serial, bool transparent_viewport);
    void ForceFallbackForTesting(const std::string& reason);
    void RequestDiagnosticCapture();
    Diagnostics diagnostics() const;

private:
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
    bool CreateSwapChain(uint32_t width, uint32_t height);
    bool EnsureSwapChainSize(uint32_t width, uint32_t height);
    bool CreatePipeline();
    bool CaptureDiagnostics(ID3D11Texture2D* back_buffer,
                            ID3D11Texture2D* flutter_texture);
    void ThreadMain();
    bool CompositeLatest();
    void SignalWork();
    void EnterFallback(const std::string& reason);
    void PublishState(Phase phase, const std::string& reason);
    static const char* PhaseName(Phase phase);

    HWND hwnd_ = nullptr;
    void* flutter_view_ = nullptr;
    std::weak_ptr<vr::NativePlayer> player_;
    double sdr_white_scale_ = 1.0;
    EngineApi engine_api_;
    StateCallback state_callback_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swap_chain_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> back_buffer_rtv_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stop_ = false;
    bool work_pending_ = false;
    bool diagnostic_capture_pending_ = true;
    bool terminal_inactive_ = false;
    bool fallback_finish_pending_ = false;
    Phase phase_ = Phase::Inactive;
    uint64_t state_serial_ = 0;
    uint64_t ack_serial_ = 0;
    double viewport_[4] = {0.0, 0.0, 1.0, 1.0};
    Diagnostics diagnostics_;
};
