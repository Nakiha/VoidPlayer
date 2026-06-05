#pragma once

#include "renderer/renderer_api_types.h"

#include <atomic>
#include <mutex>

namespace vr {

// Lock contract:
// - Owns only the renderer event callback mutex and callback storage.
// - Does not take renderer state/device/texture locks.
// - Invokes host callbacks after releasing its internal mutex.
class RendererEventBus {
public:
    void set_callback(RendererEventCallback callback);
    void clear();
    bool has_callback() const;
    void emit(const RendererEvent& event, const std::atomic<bool>& shutting_down) const;

private:
    mutable std::mutex mutex_;
    RendererEventCallback callback_;
};

} // namespace vr
