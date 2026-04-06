#pragma once

#include <flutter/plugin_registrar_windows.h>
#include <flutter/texture_registrar.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include "video_renderer/renderer.h"

#include <memory>
#include <mutex>

/// Process-global renderer pointer — allows any engine's plugin to query stats.
namespace vr { class Renderer; }
extern vr::Renderer* g_global_renderer;

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
    NakiVrTrackStats tracks[kMaxTracksFFI];
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
    void CreateRenderer(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void DestroyRenderer(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void AddTrack(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void RemoveTrack(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

    std::unique_ptr<vr::Renderer> renderer_;
    int64_t texture_id_ = -1;
    std::unique_ptr<flutter::TextureVariant> texture_variant_;
    FlutterDesktopGpuSurfaceDescriptor surface_descriptor_ = {};
    flutter::TextureRegistrar* texture_registrar_;
    IDXGIAdapter* dxgi_adapter_;
    int texture_width_ = 0;
    int texture_height_ = 0;
    std::string logs_dir_;
};
