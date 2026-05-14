#pragma once

#include <flutter/plugin_registrar_windows.h>
#include <flutter/method_channel.h>
#include <flutter/event_sink.h>
#include <flutter/standard_method_codec.h>

#include "file_picker_service.h"
#include "flutter_texture_bridge.h"
#include "native_diagnostics_ffi.h"
#include "player/native_player.h"
#include "native_diagnostics_provider.h"
#include "native_logging_bootstrap.h"
#include "native_player_method_dispatcher.h"
#include "native_player_registry.h"
#include "viewport_capture_service.h"

#include <cstdint>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <wrl/client.h>

/// Returns pointer to a static NakiVrDiagnostics (valid until next call).
extern "C" __declspec(dllexport)
const NakiVrDiagnostics* naki_vr_get_diagnostics();

class VideoRendererPlugin : public flutter::Plugin {
public:
    static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

    VideoRendererPlugin(flutter::PluginRegistrarWindows* registrar,
                        flutter::TextureRegistrar* texture_registrar,
                        IDXGIAdapter* dxgi_adapter);
    ~VideoRendererPlugin() override;

    VideoRendererPlugin(const VideoRendererPlugin&) = delete;
    VideoRendererPlugin& operator=(const VideoRendererPlugin&) = delete;

    void DrainEventQueue();

private:
    void RegisterMethodHandlers();
    void HandleMethodCall(
        const flutter::MethodCall<flutter::EncodableValue>& method_call,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

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
    void SetViewportBackgroundColor(
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
    void GetLayout(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void SetEventSink(std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> sink);
    void ClearEventSink();
    void QueueRendererEvent(const vr::RendererEvent& event);
    void RegisterEventDrainWindowProc();

    std::shared_ptr<vr::NativePlayer> player_;
    FlutterTextureBridge texture_bridge_;
    HWND event_hwnd_ = nullptr;
    std::mutex event_mutex_;
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;
    std::deque<flutter::EncodableValue> pending_events_;
    std::atomic<int64_t> event_sequence_{0};
    Microsoft::WRL::ComPtr<IDXGIAdapter> dxgi_adapter_;
    NativeDiagnosticsProvider diagnostics_;
    NativePlayerMethodDispatcher method_dispatcher_;
    FilePickerService file_picker_;
    NativeLoggingBootstrap logging_bootstrap_;
    ViewportCaptureService viewport_capture_;
};
