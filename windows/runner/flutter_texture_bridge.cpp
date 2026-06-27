#include "flutter_texture_bridge.h"

FlutterTextureBridge::FlutterTextureBridge(
    flutter::TextureRegistrar* texture_registrar)
    : texture_registrar_(texture_registrar) {}

FlutterTextureBridge::~FlutterTextureBridge() {
    Unregister();
}

bool FlutterTextureBridge::Register(
    const std::shared_ptr<vr::NativePlayer>& player) {
    Unregister();
    if (!texture_registrar_ || !player) {
        return false;
    }
    player_ = player;
    texture_id_.store(-1, std::memory_order_release);
    return true;
}

void FlutterTextureBridge::DetachFrameCallback() {
}

void FlutterTextureBridge::Unregister() {
    DetachFrameCallback();
    const int64_t registered_id =
        texture_id_.exchange(-1, std::memory_order_acq_rel);
    if (registered_id >= 0 && texture_registrar_) {
        texture_registrar_->UnregisterTexture(registered_id);
    }
    player_.reset();
}

int64_t FlutterTextureBridge::texture_id() const {
    return texture_id_.load(std::memory_order_acquire);
}
