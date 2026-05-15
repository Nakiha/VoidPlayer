#include "native_player_registry.h"

void NativeDiagnosticsSession::PublishPlayer(
    const std::shared_ptr<vr::NativePlayer>& player) {
    std::lock_guard lock(mutex_);
    player_ = player;
}

void NativeDiagnosticsSession::ClearPlayer() {
    std::lock_guard lock(mutex_);
    player_.reset();
}

std::shared_ptr<vr::NativePlayer> NativeDiagnosticsSession::PinPlayer() const {
    std::lock_guard lock(mutex_);
    return player_.lock();
}

void NativeDiagnosticsSessionRegistry::Publish(
    const std::shared_ptr<NativeDiagnosticsSession>& session) {
    std::lock_guard lock(mutex_);
    session_ = session;
}

void NativeDiagnosticsSessionRegistry::Clear(
    const std::shared_ptr<NativeDiagnosticsSession>& session) {
    std::lock_guard lock(mutex_);
    if (session_.lock() == session) {
        session_.reset();
    }
}

std::shared_ptr<NativeDiagnosticsSession>
NativeDiagnosticsSessionRegistry::PinSession() const {
    std::lock_guard lock(mutex_);
    return session_.lock();
}

NativeDiagnosticsSessionRegistry& GlobalNativeDiagnosticsSessionRegistry() {
    static NativeDiagnosticsSessionRegistry registry;
    return registry;
}
