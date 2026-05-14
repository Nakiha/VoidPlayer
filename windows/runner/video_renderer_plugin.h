#pragma once

#include <flutter/plugin_registrar_windows.h>
#include <flutter/method_channel.h>
#include <flutter/event_sink.h>
#include <flutter/standard_method_codec.h>

#include "flutter_texture_bridge.h"
#include "player/native_player.h"
#include "native_diagnostics_provider.h"
#include "native_logging_bootstrap.h"
#include "viewport_capture_service.h"

#include <cstdint>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <wrl/client.h>

/// Process-global player pointer — allows any engine's plugin to query stats.
namespace vr { class NativePlayer; }
extern std::weak_ptr<vr::NativePlayer> g_player_weak;
extern std::mutex g_player_mutex;

/// Pin the global player into a shared_ptr. Returns nullptr if not alive.
std::shared_ptr<vr::NativePlayer> pin_global_player();

/// ---- dart:ffi flat struct for diagnostics (no heap, no string) ----

constexpr int kMaxTracksFFI = 4;

struct NakiVrTrackStats {
    int32_t  slot;            // -1 = unused
    int32_t  file_id;
    double   fps;
    double   avg_decode_ms;
    double   max_decode_ms;
    int32_t  buffer_count;
    int32_t  buffer_capacity;
    int32_t  buffer_state;    // TrackState enum value
    uint64_t cpu_frame_memory_bytes;
    uint64_t packet_queue_memory_bytes;
    int64_t  current_pts_us;
    int64_t  current_dts_us;
};

struct NakiVrDiagnostics {
    double   playback_time_s;
    int32_t  is_playing;
    int32_t  track_count;
    uint64_t process_working_set_bytes;
    uint64_t process_private_bytes;
    uint64_t dedicated_video_memory_bytes;
    uint64_t cpu_frame_memory_bytes;
    uint64_t packet_queue_memory_bytes;
    NakiVrTrackStats tracks[kMaxTracksFFI];
    int32_t d3d_device_lost;
    int32_t reserved0;
    int64_t d3d_device_removed_reason;
};

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
    void PickFiles(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void CaptureViewport(
        const flutter::EncodableValue* arguments,
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
    NativeLoggingBootstrap logging_bootstrap_;
    ViewportCaptureService viewport_capture_;
};
