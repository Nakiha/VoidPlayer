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
#ifdef _WIN32
    bool active_ = false;
#endif
};

std::string current_render_thread_id_string();

} // namespace vr
