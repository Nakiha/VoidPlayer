#include "native_player_registry.h"

void NativePlayerRegistry::Publish(
    const std::shared_ptr<vr::NativePlayer>& player) {
    std::lock_guard lock(mutex_);
    player_ = player;
}

void NativePlayerRegistry::Clear() {
    std::lock_guard lock(mutex_);
    player_.reset();
}

std::shared_ptr<vr::NativePlayer> NativePlayerRegistry::Pin() const {
    std::lock_guard lock(mutex_);
    return player_.lock();
}

NativePlayerRegistry& GlobalNativePlayerRegistry() {
    static NativePlayerRegistry registry;
    return registry;
}
