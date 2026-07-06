#pragma once

#include <flutter/plugin_registrar_windows.h>
#include <flutter/method_channel.h>
#include <flutter/event_sink.h>
#include <flutter/standard_method_codec.h>

#include "file_picker_service.h"
#include "flutter_texture_bridge.h"
#include "native_diagnostics_ffi.h"
#include "windows/player/native_player.h"
#include "native_diagnostics_provider.h"
#include "native_logging_bootstrap.h"
#include "native_player_method_dispatcher.h"
#include "native_player_registry.h"
#include "renderer_event_bridge.h"
#include "viewport_capture_service.h"
#include "window_capture_service.h"
#include "windows/presentation/windows_display_resolver.h"
#include "windows/presentation/windows_device_recovery.h"
#include "windows/presentation/windows_presentation_policy.h"
#include "windows_native_compositor.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <wrl/client.h>

/// Returns pointer to a static NakiVrDiagnostics (valid until next call).
extern "C" __declspec(dllexport)
const NakiVrDiagnostics* naki_vr_get_diagnostics();

struct PlatformTaskState;

class VideoRendererPlugin : public flutter::Plugin {
public:
    static void RegisterWithRegistrar(
        flutter::PluginRegistrarWindows* registrar,
        FlutterDesktopPluginRegistrarRef core_registrar);

    VideoRendererPlugin(flutter::PluginRegistrarWindows* registrar,
                        flutter::TextureRegistrar* texture_registrar,
                        IDXGIAdapter* dxgi_adapter,
                        HWND window_handle,
                        void* flutter_view_handle);
    ~VideoRendererPlugin() override;

    VideoRendererPlugin(const VideoRendererPlugin&) = delete;
    VideoRendererPlugin& operator=(const VideoRendererPlugin&) = delete;

private:
    void RegisterMethodHandlers();
    void HandleMethodCall(
        const flutter::MethodCall<flutter::EncodableValue>& method_call,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    std::optional<LRESULT> HandleTopLevelWindowProc(
        HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void PostPlatformTask(std::function<void()> task);
    void DrainPlatformTasks();

    void InitLogging(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void CreatePlayer(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void DestroyPlayer(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void AddTrack(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void RemoveTrack(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void SetTrackOffset(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void SetLoopRange(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void SetAudibleTrack(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void Play(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void Pause(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void Seek(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void Resize(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void PrewarmNativePresentationTargetSize(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void SetViewportBackgroundColor(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void SetNativeCompositorViewportRect(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void RequestNativeCompositorFlutterFrame(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void BoostNativeCompositorFlutterInteraction(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void PrepareNativeCompositorSourceCache(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void ClearNativeCompositorSourceCache(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void SetNativeAnalysisOverlay(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void AckNativeCompositorFlutterState(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void DebugFailNativeCompositor(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void DebugSimulateWindowsDeviceLoss(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void ResetNativePerfCounters(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void BeginNativeInteractionSample(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void EndNativeInteractionSample(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void SetSpeed(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void StepForward(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void StepBackward(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void CurrentPts(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void CurrentPresentedFrame(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void Duration(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void IsPlaying(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void GetPlaybackSnapshot(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void ApplyLayout(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void GetTracks(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void GetDiagnostics(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void PickFiles(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void CaptureViewport(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void CaptureViewportRegion(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void CaptureWindow(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void GetLayout(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void SetEventSink(std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink);
    void ClearEventSink();
    void QueueRendererEvent(const vr::RendererEvent& event);
    void ScheduleDisplayPolicyRefresh();
    vr::WindowsDisplayProbeSnapshot RefreshPresentationPolicy(
        const char* trigger,
        bool allow_transient_hold = true);

    std::shared_ptr<vr::NativePlayer> player_;
    FlutterTextureBridge texture_bridge_;
    RendererEventBridge event_bridge_;
    std::shared_ptr<NativeDiagnosticsSession> diagnostics_session_;
    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgi_adapter_;
    NativeDiagnosticsProvider diagnostics_;
    NativePlayerMethodDispatcher method_dispatcher_;
    FilePickerService file_picker_;
    NativeLoggingBootstrap logging_bootstrap_;
    ViewportCaptureService viewport_capture_;
    WindowCaptureService window_capture_;
    flutter::PluginRegistrarWindows* registrar_ = nullptr;
    int window_proc_delegate_id_ = -1;
    std::shared_ptr<PlatformTaskState> platform_task_state_;
    HWND window_handle_ = nullptr;
    vr::WindowsDisplayResolver display_resolver_;
    vr::WindowsDisplayProbeTracker display_probe_tracker_;
    vr::WindowsPresentationPolicy presentation_policy_;
    std::unique_ptr<WindowsNativeCompositor> native_compositor_;
    std::string native_compositor_source_signature_;
    std::string native_compositor_source_failure_signature_;
    vr::WindowsDeviceRecoveryDiagnostics device_recovery_;
    void* flutter_view_handle_ = nullptr;
    std::string presentation_sdr_white_level_status_ = "nominal-default";
    std::string presentation_request_;
    uint64_t presentation_locked_display_generation_ = 0;
    std::string presentation_locked_output_identity_;
    int64_t presentation_locked_sdr_white_level_milli_nits_ = 80000;
};
