#pragma once

#include <flutter/plugin_registrar_windows.h>
#include <flutter/texture_registrar.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include "video_renderer/renderer.h"

#include <memory>
#include <mutex>

class VideoRendererPlugin : public flutter::Plugin {
public:
    static void RegisterWithRegistrar(flutter::PluginRegistrarWindows* registrar);

    VideoRendererPlugin(flutter::TextureRegistrar* texture_registrar,
                        IDXGIAdapter* dxgi_adapter);
    ~VideoRendererPlugin() override;

    // Prevent copying
    VideoRendererPlugin(const VideoRendererPlugin&) = delete;
    VideoRendererPlugin& operator=(const VideoRendererPlugin&) = delete;

private:
    void HandleMethodCall(
        const flutter::MethodCall<flutter::EncodableValue>& method_call,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

    void CreateRenderer(
        const flutter::EncodableValue* arguments,
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);
    void DestroyRenderer(
        std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

    // State
    std::unique_ptr<vr::Renderer> renderer_;
    int64_t texture_id_ = -1;
    std::unique_ptr<flutter::TextureVariant> texture_variant_;
    FlutterDesktopGpuSurfaceDescriptor surface_descriptor_ = {};
    flutter::TextureRegistrar* texture_registrar_;
    IDXGIAdapter* dxgi_adapter_;
    int texture_width_ = 0;
    int texture_height_ = 0;
};
