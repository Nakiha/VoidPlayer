#pragma once

#include <string>

namespace vr {

class ScopedRenderThreadTiming {
public:
    ScopedRenderThreadTiming();
    ~ScopedRenderThreadTiming();

    ScopedRenderThreadTiming(const ScopedRenderThreadTiming&) = delete;
    ScopedRenderThreadTiming& operator=(const ScopedRenderThreadTiming&) = delete;

private:
    bool active_ = false;
};

std::string current_render_thread_id_string();

} // namespace vr
