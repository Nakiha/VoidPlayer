#pragma once

#include <flutter/texture_registrar.h>
#include "windows/player/native_player.h"

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
    flutter::TextureRegistrar* texture_registrar_;
    std::weak_ptr<vr::NativePlayer> player_;
    std::atomic<int64_t> texture_id_{-1};
};
