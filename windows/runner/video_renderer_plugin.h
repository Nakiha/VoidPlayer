#pragma once

#include <flutter/plugin_registrar_windows.h>
#include <flutter/texture_registrar.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include "player/native_player.h"

#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>

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
};

struct NakiVrDiagnostics {
    double   playback_time_s;
    int32_t  is_playing;
    int32_t  track_count;
    uint64_t process_working_set_bytes;
    uint64_t process_private_bytes;
    uint64_t dedicated_video_memory_bytes;
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

    VideoRendererPlugin(flutter::TextureRegistrar* texture_registrar,
                        IDXGIAdapter* dxgi_adapter);
    ~VideoRendererPlugin() override;

    VideoRendererPlugin(const VideoRendererPlugin&) = delete;
    VideoRendererPlugin& operator=(const VideoRendererPlugin&) = delete;

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

    std::shared_ptr<vr::NativePlayer> player_;
    std::atomic<int64_t> texture_id_{-1};
    std::unique_ptr<flutter::TextureVariant> texture_variant_;
    FlutterDesktopGpuSurfaceDescriptor surface_descriptor_ = {};
    flutter::TextureRegistrar* texture_registrar_;
    IDXGIAdapter* dxgi_adapter_;
    std::string logs_dir_;
    std::string log_file_name_;
};
