#pragma once

#include <memory>
#include <mutex>

namespace vr {
class NativePlayer;
}

class NativePlayerRegistry {
public:
    void Publish(const std::shared_ptr<vr::NativePlayer>& player);
    void Clear();
    std::shared_ptr<vr::NativePlayer> Pin() const;

private:
    mutable std::mutex mutex_;
    std::weak_ptr<vr::NativePlayer> player_;
};

NativePlayerRegistry& GlobalNativePlayerRegistry();
