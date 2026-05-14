#pragma once

#include <flutter/texture_registrar.h>
#include <flutter_windows.h>

#include "player/native_player.h"

#include <atomic>
#include <cstdint>
#include <memory>

class FlutterTextureBridge {
public:
    explicit FlutterTextureBridge(flutter::TextureRegistrar* texture_registrar);
    ~FlutterTextureBridge();

    FlutterTextureBridge(const FlutterTextureBridge&) = delete;
    FlutterTextureBridge& operator=(const FlutterTextureBridge&) = delete;

    bool Register(const std::shared_ptr<vr::NativePlayer>& player);
    void DetachFrameCallback();
    void Unregister();
    int64_t texture_id() const;

private:
    const FlutterDesktopGpuSurfaceDescriptor* AcquireSurfaceDescriptor(
        size_t width,
        size_t height);
    void MarkFrameAvailable();

    flutter::TextureRegistrar* texture_registrar_;
    std::weak_ptr<vr::NativePlayer> player_;
    std::atomic<int64_t> texture_id_{-1};
    std::unique_ptr<flutter::TextureVariant> texture_variant_;
    FlutterDesktopGpuSurfaceDescriptor surface_descriptor_ = {};
    bool frame_callback_attached_ = false;
};
