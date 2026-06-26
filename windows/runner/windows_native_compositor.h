#pragma once

#include "windows/d3d11/shared_fp16_ring.h"
#include "windows/d3d11/shared_source_cache_ring.h"
#include "windows/d3d11/cross_adapter_transport.h"
#include "windows/presentation/windows_dcomp_composite.h"
#include "windows/presentation/windows_device_recovery.h"
#include "windows/presentation/windows_high_refresh_metrics.h"
#include "windows/presentation/windows_overlay_layer_state.h"
#include "windows/player/native_player.h"

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d12.h>
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
#include <vector>

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
        Failed,
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
        std::string producer_adapter_luid = "0:0";
        std::string output_adapter_luid = "0:0";
        std::string pending_output_adapter_luid = "0:0";
        std::string cross_adapter_transport_mode = "same-adapter";
        std::string cross_adapter_transport_status = "not-required";
        std::string cross_adapter_sync_kind = "keyed-mutex";
        std::string cross_adapter_requested_sync_kind = "auto";
        std::string cross_adapter_active_sync_kind = "keyed-mutex";
        std::string cross_adapter_sync_fallback_reason = "none";
        std::string cross_adapter_last_error = "none";
        std::string device_recovery_state = "stable";
        std::string device_recovery_last_reason = "none";
        std::string device_recovery_last_removed_reason = "0x00000000";
        std::string device_recovery_fallback_stage = "none";
        uint64_t state_serial = 0;
        uint64_t ack_serial = 0;
        uint64_t transition_serial = 0;
        uint64_t device_recovery_generation = 0;
        uint64_t device_recovery_attempt_count = 0;
        uint64_t device_recovery_success_count = 0;
        uint64_t device_recovery_failure_count = 0;
        uint64_t device_recovery_last_duration_ms = 0;
        uint64_t output_generation = 0;
        uint64_t output_migration_count = 0;
        uint64_t output_migration_failure_count = 0;
        uint64_t hdr_promotion_count = 0;
        uint64_t hdr_demotion_count = 0;
        uint64_t target_fallback_count = 0;
        uint64_t transport_generation = 0;
        uint64_t transport_copy_count = 0;
        uint64_t transport_copy_bytes = 0;
        uint64_t transport_fence_wait_count = 0;
        uint64_t transport_timeout_count = 0;
        uint64_t transport_last_copy_us = 0;
        uint64_t transport_total_copy_us = 0;
        uint64_t shared_fence_signal_count = 0;
        uint64_t shared_fence_wait_count = 0;
        uint64_t shared_fence_timeout_count = 0;
        uint64_t shared_fence_last_wait_us = 0;
        uint64_t shared_fence_p95_wait_us = 0;
        uint64_t event_query_p95_wait_us = 0;
        uint64_t flutter_transport_generation = 0;
        uint64_t video_transport_generation = 0;
        uint64_t source_transport_generation = 0;
        uint64_t flutter_generation = 0;
        uint64_t flutter_export_state_generation = 0;
        uint64_t flutter_export_ring_generation = 0;
        uint64_t flutter_export_publish_count = 0;
        uint64_t flutter_export_request_count = 0;
        uint64_t flutter_export_request_dispatch_count = 0;
        uint64_t flutter_export_schedule_frame_count = 0;
        uint64_t flutter_export_vsync_count = 0;
        uint64_t flutter_export_present_count = 0;
        uint64_t flutter_export_begin_count = 0;
        uint64_t flutter_export_begin_fail_count = 0;
        uint64_t flutter_export_make_current_fail_count = 0;
        uint64_t flutter_export_publish_fail_count = 0;
        uint64_t flutter_export_flush_count = 0;
        uint64_t flutter_export_finish_count = 0;
        uint64_t flutter_export_backpressure_count = 0;
        uint64_t flutter_export_pending_frame_pump_frames = 0;
        uint64_t flutter_export_stale_timeout_count = 0;
        uint64_t flutter_export_unsolicited_signal_count = 0;
        uint64_t flutter_export_unsolicited_throttle_count = 0;
        uint64_t flutter_d3d11_srv_available_count = 0;
        uint64_t flutter_d3d11_srv_unavailable_count = 0;
        uint64_t flutter_d3d11_srv_forced_skip_count = 0;
        uint64_t flutter_d3d11_srv_lazy_create_count = 0;
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
        int64_t high_refresh_display_hz = 60;
        int64_t dcomp_present_interval_p95_us = 0;
        int64_t dcomp_composite_p95_us = 0;
        int64_t dcomp_draw_p95_us = 0;
        int64_t dcomp_present_block_p95_us = 0;
        int64_t dcomp_acquire_wait_p95_us = 0;
        int64_t interaction_input_to_present_p95_us = 0;
        int64_t dcomp_drop_rate_x1000 = 0;
        uint64_t source_projection_reuse_count = 0;
        uint64_t viewport_redraw_during_projection_count = 0;
        uint64_t overlay_layer_raster_count = 0;
        uint64_t overlay_layer_upload_count = 0;
        uint64_t overlay_layer_reuse_count = 0;
        uint64_t overlay_layer_texture_count = 0;
        uint64_t overlay_layer_bytes = 0;
        uint64_t overlay_layer_generation = 0;
        uint64_t overlay_layer_committed_generation = 0;
        uint64_t overlay_layer_pending_generation = 0;
        uint64_t overlay_layer_composite_count = 0;
        uint64_t overlay_layer_miss_count = 0;
        uint64_t overlay_layer_backpressure_count = 0;
        int64_t overlay_composite_p95_us = 0;
        int64_t overlay_raster_p95_us = 0;
        int64_t overlay_upload_p95_us = 0;
        int64_t hot_path_display_hz = 60;
        int64_t hot_path_frame_budget_us = 16666;
        int64_t hot_path_present_interval_p95_us = 0;
        int64_t hot_path_composite_p95_us = 0;
        int64_t hot_path_draw_p95_us = 0;
        int64_t hot_path_present_block_p95_us = 0;
        int64_t hot_path_acquire_wait_p95_us = 0;
        int64_t hot_path_input_to_present_p95_us = 0;
        int64_t hot_path_drop_rate_x1000 = 0;
        uint64_t hot_path_projection_only_update_count = 0;
        uint64_t hot_path_viewport_redraw_during_projection_count = 0;
        uint64_t hot_path_source_cache_reuse_count = 0;
        uint64_t hot_path_overlay_reuse_count = 0;
        uint64_t hot_path_overlay_raster_count = 0;
        uint64_t hot_path_overlay_upload_count = 0;
        double source_cache_hz = 0.0;
        double source_projection_hz = 0.0;
        std::string overlay_layer_mode = "inactive";
        std::string overlay_layer_fallback_reason = "none";
        std::string overlay_layer_last_error = "none";
        std::string high_refresh_gate_last_result = "not-run";
        std::string hot_path_mode = "inactive";
        std::string hot_path_last_failure_reason = "none";
        std::string hot_path_gate_result = "not-run";
        std::string retained_graph_mode = "inactive";
        std::string retained_graph_fallback_reason = "none";
        uint32_t swap_chain_width = 0;
        uint32_t swap_chain_height = 0;
        bool engine_export_available = false;
        bool engine_export_frame_pump_available = false;
        bool flutter_export_latest_available = false;
        bool flutter_d3d11_srv_forced_disabled = false;
        bool swap_chain_active = false;
        bool color_space_supported = false;
        bool sdr_tone_map_active = true;
        bool cross_adapter_required = false;
        bool cross_adapter_supported = false;
        bool device_recovery_preserved_player = true;
        bool device_recovery_last_frame_held = false;
        bool transport_bgra8_supported = false;
        bool transport_fp16_supported = false;
        bool transport_shared_fence_supported = false;
        bool transport_shared_fence_producer_supported = false;
        bool transport_shared_fence_output_supported = false;
        bool transport_shared_fence_handle_created = false;
        bool transport_shared_fence_open_succeeded = false;
        bool source_projection_enabled = false;
        bool source_cache_active = false;
        bool high_refresh_gate_supported = false;
        bool overlay_retained_layer_active = false;
        bool hot_path_active = false;
        bool retained_graph_active = false;
        uint64_t retained_graph_commit_count = 0;
        uint64_t retained_graph_projection_commit_count = 0;
        uint64_t retained_graph_source_bake_count = 0;
        uint64_t retained_graph_flutter_bake_count = 0;
        uint64_t retained_graph_projection_skip_present_count = 0;
        uint64_t retained_graph_deferred_content_count = 0;
        uint64_t retained_graph_commit_defer_count = 0;
        int64_t retained_graph_flutter_bake_p95_us = 0;
        int64_t retained_graph_source_bake_p95_us = 0;
        int64_t retained_graph_apply_p95_us = 0;
        int64_t retained_graph_commit_p95_us = 0;
    };

    using StateCallback = std::function<void(Phase, uint64_t, const std::string&)>;
    using SourceProjection = vr::WindowsSourceProjection;

    WindowsNativeCompositor();
    ~WindowsNativeCompositor();

    bool Start(HWND hwnd,
               void* flutter_view,
               const std::shared_ptr<vr::NativePlayer>& player,
               IDXGIAdapter* producer_adapter,
               IDXGIAdapter* output_adapter,
               double sdr_white_level_nits,
               OutputTarget output_target,
               StateCallback callback);
    void Stop(const char* reason = "shutdown");
    void SetViewportRect(double left, double top, double right, double bottom);
    void SetViewportBackgroundColor(uint32_t argb);
    bool RequestFlutterFrame(const std::string& reason);
    void BoostFlutterInteraction(const std::string& reason);
    void SetSourceProjection(const SourceProjection& projection);
    void DisableRetainedSourceProjection(const std::string& reason);
    void ClearSourceProjection(const std::string& reason);
    void SetSourceCacheError(const std::string& error);
    void NotifySourceCachePublished();
    void RequestOutputTarget(OutputTarget target,
                             IDXGIAdapter* output_adapter,
                             double sdr_white_level_nits,
                             uint64_t display_generation,
                             const std::string& reason);
    void AcknowledgeFlutterState(uint64_t serial, bool transparent_viewport);
    void ForceFailureForTesting(const std::string& reason);
    bool BeginDeviceRecovery(const std::string& reason, long removed_reason);
    void RequestDiagnosticCapture();
    void SetHighRefreshDisplayHz(int64_t display_hz);
    void ResetHighRefreshMetrics();
    void BeginInteractionSample(const std::string& label);
    void EndInteractionSample(const std::string& label);
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

    enum class FlutterSurfaceBackend : int {
        Unknown = 0,
        D3D11 = 1,
        D3D12 = 2,
    };

    enum class FlutterSurfaceSync : int {
        None = 0,
        KeyedMutex = 1,
        SharedFence = 2,
    };

    struct FlutterSurface {
        size_t struct_size = sizeof(FlutterSurface);
        FlutterSurfaceBackend backend = FlutterSurfaceBackend::D3D11;
        FlutterSurfaceSync sync = FlutterSurfaceSync::KeyedMutex;
        HANDLE shared_texture_handle = nullptr;
        HANDLE fence_handle = nullptr;
        uint64_t fence_value = 0;
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

    struct FlutterSurfaceAcquireOptions {
        size_t struct_size = sizeof(FlutterSurfaceAcquireOptions);
        FlutterSurfaceBackend requested_backend = FlutterSurfaceBackend::Unknown;
    };

    struct FlutterSurfaceV2 {
        size_t struct_size = sizeof(FlutterSurfaceV2);
        FlutterSurfaceBackend backend = FlutterSurfaceBackend::Unknown;
        FlutterSurfaceSync sync = FlutterSurfaceSync::None;
        HANDLE texture_handle = nullptr;
        HANDLE fence_handle = nullptr;
        uint64_t fence_value = 0;
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

    struct FlutterSurfaceExportState {
        size_t struct_size = sizeof(FlutterSurfaceExportState);
        int mode = 0;
        uint64_t ring_generation = 0;
        uint64_t frame_generation = 0;
        uint64_t publish_count = 0;
        uint64_t request_count = 0;
        uint64_t request_dispatch_count = 0;
        uint64_t schedule_frame_count = 0;
        uint64_t vsync_count = 0;
        uint64_t present_count = 0;
        uint64_t export_begin_count = 0;
        uint64_t export_begin_fail_count = 0;
        uint64_t export_make_current_fail_count = 0;
        uint64_t export_publish_fail_count = 0;
        uint64_t export_flush_count = 0;
        uint64_t export_finish_count = 0;
        uint64_t backpressure_count = 0;
        uint64_t pending_frame_pump_frames = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t latest_slot = 0;
        bool latest_available = false;
        bool shutdown = false;
        uint64_t last_request_time_us = 0;
        uint64_t last_request_dispatch_time_us = 0;
        uint64_t last_schedule_frame_time_us = 0;
        uint64_t last_vsync_time_us = 0;
        uint64_t last_present_time_us = 0;
        uint64_t last_begin_time_us = 0;
        uint64_t last_begin_fail_time_us = 0;
        uint64_t last_backpressure_time_us = 0;
        uint64_t last_publish_time_us = 0;
        uint64_t last_export_sync_time_us = 0;
        uint64_t last_acquire_time_us = 0;
        uint64_t last_release_time_us = 0;
        uint32_t active_lease_count = 0;
        uint32_t writing_slot_count = 0;
        uint32_t leased_slot_count = 0;
        uint32_t retired_ring_count = 0;
        uint32_t latest_slot_lease_count = 0;
        uint64_t acquire_count = 0;
        uint64_t release_count = 0;
    };

    struct RetainedSurfaceLayer {
        Microsoft::WRL::ComPtr<IDCompositionSurface> surface;
        Microsoft::WRL::ComPtr<IDCompositionVisual> visual;
        Microsoft::WRL::ComPtr<IDCompositionRectangleClip> clip;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t generation = 0;
        bool ready = false;
    };

    using SetExportModeFn = bool (*)(void*, int);
    using RequestSurfaceExportFrameFn = bool (*)(void*);
    using GetSurfaceExportStateFn =
        bool (*)(void*, FlutterSurfaceExportState*);
    using PublishedCallback = void (*)(void*, uint64_t, void*);
    using SetPublishedCallbackFn = void (*)(void*, PublishedCallback, void*);
    using AcquireFlutterSurfaceV2Fn =
        bool (*)(void*, const FlutterSurfaceAcquireOptions*, FlutterSurfaceV2*);
    using ReleaseFlutterSurfaceFn = bool (*)(void*, uint64_t);

    struct EngineApi {
        SetExportModeFn set_mode = nullptr;
        RequestSurfaceExportFrameFn request_frame = nullptr;
        GetSurfaceExportStateFn get_state = nullptr;
        SetPublishedCallbackFn set_callback = nullptr;
        AcquireFlutterSurfaceV2Fn acquire_v2 = nullptr;
        ReleaseFlutterSurfaceFn release = nullptr;
        bool available() const {
            return set_mode && set_callback && acquire_v2 && release;
        }
        bool frame_pump_available() const {
            return request_frame && get_state;
        }
    };

    static void OnFlutterSurfacePublished(
        void* view, uint64_t generation, void* user_data);
    bool LoadEngineApi();
    bool InitializeDeviceAndComposition(IDXGIAdapter* producer_adapter,
                                        IDXGIAdapter* output_adapter);
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
    void ResetOverlayLayer(const std::string& reason);
    void ResetRetainedGraph(const std::string& reason);
    bool CanUseRetainedGraph(const SourceProjection& projection,
                             OutputTarget target) const;
    bool EnsureRetainedGraph(uint32_t width,
                             uint32_t height,
                             OutputTarget target);
    bool BakeRetainedSourceSurface(size_t slot,
                                   ID3D11ShaderResourceView* source_srv,
                                   uint32_t width,
                                   uint32_t height,
                                   OutputTarget target,
                                   int color_transfer,
                                   uint64_t generation,
                                   const std::shared_ptr<
                                       const vr::AnalysisOverlayPrimitivePackage>&
                                       overlay);
    bool BakeRetainedFlutterSurface(const FlutterSurface& surface,
                                    OutputTarget target,
                                    ID3D11ShaderResourceView* flutter_srv);
    bool EnsureHeldFlutterSrv();
    bool ApplyRetainedProjection(uint32_t width,
                                 uint32_t height,
                                 OutputTarget target,
                                 const SourceProjection& projection);
    bool ShouldDeferRetainedGraphCommitLocked(
        std::chrono::steady_clock::time_point now,
        bool projection_only);
    void RecordInteractionCommitLatencyLocked(
        std::chrono::steady_clock::time_point committed_at);
    bool CommitRetainedGraph(const char* reason);
    void ReleaseHeldInputs(const std::shared_ptr<vr::NativePlayer>& player);
    void ThreadMain();
    bool CompositeLatest();
    void SignalWork();
    void EnterFailed(const std::string& reason);
    void PublishState(Phase phase, const std::string& reason);
    bool EnsureProducerDevice(IDXGIAdapter* producer_adapter);
    bool SetOutputAdapterLocked(IDXGIAdapter* output_adapter);
    bool IsCrossAdapterActive() const;
    bool OpenInputTexture(ID3D11Device1* device1,
                          HANDLE handle,
                          ID3D11Texture2D** texture) const;
    bool TransportInput(ID3D11Texture2D* producer_texture,
                        DXGI_FORMAT format,
                        uint32_t width,
                        uint32_t height,
                        vr::D3D11CrossAdapterTextureTransport& transport,
                        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& srv);
    void UpdateTransportDiagnosticsLocked();
    static const char* PhaseName(Phase phase);
    static const char* OutputTargetName(OutputTarget target);
    static const char* OutputFormatName(OutputTarget target);
    static const char* OutputColorSpaceName(OutputTarget target);

    HWND hwnd_ = nullptr;
    void* flutter_view_ = nullptr;
    std::weak_ptr<vr::NativePlayer> player_;
    std::atomic<double> sdr_white_scale_{1.0};
    uint64_t locked_display_generation_ = 0;
    int32_t producer_luid_high_ = 0;
    uint32_t producer_luid_low_ = 0;
    int32_t output_luid_high_ = 0;
    uint32_t output_luid_low_ = 0;
    int32_t pending_output_luid_high_ = 0;
    uint32_t pending_output_luid_low_ = 0;
    EngineApi engine_api_;
    StateCallback state_callback_;

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Device> producer_device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> producer_context_;
    Microsoft::WRL::ComPtr<IDXGIAdapter> producer_adapter_;
    Microsoft::WRL::ComPtr<IDXGIAdapter> output_adapter_;
    Microsoft::WRL::ComPtr<IDXGIAdapter> pending_output_adapter_;
    vr::WindowsCrossAdapterTransportSupport transport_support_;
    vr::WindowsCrossAdapterSyncRequest cross_adapter_sync_request_ =
        vr::WindowsCrossAdapterSyncRequest::Auto;
    SwapChainResources current_swap_chain_;
    SwapChainResources pending_swap_chain_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_device_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> dcomp_target_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> dcomp_visual_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> retained_root_visual_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> retained_background_visual_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> retained_source_root_visual_;
    RetainedSurfaceLayer retained_background_;
    RetainedSurfaceLayer retained_flutter_;
    std::array<RetainedSurfaceLayer, 4> retained_sources_;
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
    vr::D3D11CrossAdapterTextureTransport video_transport_;

    bool held_sdr_video_valid_ = false;
    vr::SharedTextureSnapshot held_sdr_video_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> held_sdr_video_texture_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> held_sdr_video_srv_;
    vr::D3D11CrossAdapterTextureTransport sdr_video_transport_;

    bool held_flutter_valid_ = false;
    FlutterSurface held_flutter_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> held_flutter_texture_;
    Microsoft::WRL::ComPtr<ID3D12Resource> held_flutter_d3d12_resource_;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> held_flutter_mutex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> held_flutter_srv_;
    vr::D3D11CrossAdapterTextureTransport flutter_transport_;
    uint64_t external_flutter_surface_submitted_generation_ = 0;
    uint64_t external_flutter_surface_refresh_generation_ = 0;
    bool disable_flutter_d3d11_srv_ = false;

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
    std::array<vr::D3D11CrossAdapterTextureTransport, 4> source_transports_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    bool stop_ = false;
    bool work_pending_ = false;
    bool diagnostic_capture_pending_ = true;
    bool terminal_inactive_ = false;
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
    bool retained_projection_dirty_ = false;
    bool retained_source_content_dirty_ = false;
    bool retained_flutter_content_dirty_ = false;
    bool retained_graph_active_ = false;
    OutputTarget retained_graph_target_ = OutputTarget::SDR;
    uint32_t retained_graph_width_ = 0;
    uint32_t retained_graph_height_ = 0;
    uint64_t retained_graph_commit_count_ = 0;
    uint64_t retained_graph_projection_commit_count_ = 0;
    uint64_t retained_graph_source_bake_count_ = 0;
    uint64_t retained_graph_flutter_bake_count_ = 0;
    uint64_t retained_graph_projection_skip_present_count_ = 0;
    uint64_t retained_graph_deferred_content_count_ = 0;
    uint64_t retained_graph_commit_defer_count_ = 0;
    std::string retained_graph_fallback_reason_ = "none";
    std::vector<int64_t> retained_graph_flutter_bake_us_;
    std::vector<int64_t> retained_graph_source_bake_us_;
    std::vector<int64_t> retained_graph_apply_us_;
    std::vector<int64_t> retained_graph_commit_us_;
    std::chrono::steady_clock::time_point
        retained_deferred_content_deadline_{};
    std::chrono::steady_clock::time_point
        retained_graph_commit_deadline_{};
    std::chrono::steady_clock::time_point
        last_retained_graph_commit_time_{};
    std::chrono::steady_clock::time_point
        last_retained_projection_update_{};
    std::chrono::steady_clock::time_point
        last_transition_guard_log_{};
    std::chrono::steady_clock::time_point
        last_source_projection_debug_log_{};
    std::chrono::steady_clock::time_point
        last_flutter_export_pacing_log_{};
    uint64_t last_flutter_export_pacing_request_count_ = 0;
    uint64_t last_flutter_export_pacing_request_dispatch_count_ = 0;
    uint64_t last_flutter_export_pacing_schedule_frame_count_ = 0;
    uint64_t last_flutter_export_pacing_vsync_count_ = 0;
    uint64_t last_flutter_export_pacing_publish_count_ = 0;
    uint64_t last_flutter_export_pacing_present_count_ = 0;
    uint64_t last_flutter_export_pacing_acquire_count_ = 0;
    uint64_t last_flutter_export_pacing_release_count_ = 0;
    uint64_t last_flutter_export_pacing_begin_count_ = 0;
    uint64_t last_flutter_export_pacing_backpressure_count_ = 0;
    uint64_t last_flutter_export_pacing_retained_commit_count_ = 0;
    uint64_t last_flutter_export_pacing_retained_projection_count_ = 0;
    std::chrono::steady_clock::time_point rate_start_time_{};
    uint64_t source_cache_publish_count_ = 0;
    bool source_cache_base_lease_wait_logged_ = false;
    bool source_cache_bundle_acquire_logged_ = false;
    bool source_cache_consumed_logged_ = false;
    vr::WindowsHighRefreshMetrics high_refresh_metrics_;
    std::chrono::steady_clock::time_point last_present_time_{};
    std::chrono::steady_clock::time_point interaction_sample_started_{};
    uint64_t last_overlay_metrics_generation_ = 0;
    bool interaction_sample_active_ = false;
    uint32_t last_logged_backbuffer_width_ = 0;
    uint32_t last_logged_backbuffer_height_ = 0;
    uint32_t last_logged_flutter_width_ = 0;
    uint32_t last_logged_flutter_height_ = 0;
    uint32_t last_logged_video_width_ = 0;
    uint32_t last_logged_video_height_ = 0;
    uint64_t flutter_generation_log_count_ = 0;
    uint64_t flutter_publish_callback_count_ = 0;
    uint64_t last_flutter_publish_callback_generation_ = 0;
    uint64_t flutter_export_unsolicited_signal_count_ = 0;
    uint64_t flutter_export_unsolicited_throttle_count_ = 0;
    std::chrono::steady_clock::time_point
        last_unsolicited_flutter_export_signal_{};
    uint64_t flutter_frame_request_sequence_ = 0;
    uint64_t pending_flutter_frame_request_sequence_ = 0;
    uint64_t pending_flutter_frame_request_base_generation_ = 0;
    std::string pending_flutter_frame_request_reason_;
    std::chrono::steady_clock::time_point
        pending_flutter_frame_request_time_{};
    std::chrono::steady_clock::time_point
        last_explicit_flutter_frame_request_time_{};
    bool pending_flutter_frame_request_acquire_logged_ = false;
    uint64_t flutter_export_stale_timeout_count_ = 0;
    double last_logged_viewport_[4] = {-1.0, -1.0, -1.0, -1.0};
    Microsoft::WRL::ComPtr<ID3D11Buffer> overlay_vertex_buffer_;
    UINT overlay_vertex_count_ = 0;
    vr::WindowsOverlayLayerCacheState overlay_layer_state_;
    vr::WindowsOverlayLayerSignature overlay_layer_signature_;
    Diagnostics diagnostics_;
};
