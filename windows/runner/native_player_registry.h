#pragma once

#include <memory>
#include <mutex>

namespace vr {
class NativePlayer;
}

class NativeDiagnosticsSession {
public:
    void PublishPlayer(const std::shared_ptr<vr::NativePlayer>& player);
    void ClearPlayer();
    std::shared_ptr<vr::NativePlayer> PinPlayer() const;

private:
    mutable std::mutex mutex_;
    std::weak_ptr<vr::NativePlayer> player_;
};

class NativeDiagnosticsSessionRegistry {
public:
    void Publish(const std::shared_ptr<NativeDiagnosticsSession>& session);
    void Clear(const std::shared_ptr<NativeDiagnosticsSession>& session);
    std::shared_ptr<NativeDiagnosticsSession> PinSession() const;

private:
    mutable std::mutex mutex_;
    std::weak_ptr<NativeDiagnosticsSession> session_;
};

NativeDiagnosticsSessionRegistry& GlobalNativeDiagnosticsSessionRegistry();
