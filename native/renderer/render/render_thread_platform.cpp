#include "renderer/render/render_thread_platform.h"

#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace vr {

ScopedRenderThreadTiming::ScopedRenderThreadTiming() {
#ifdef _WIN32
    active_ = timeBeginPeriod(1) == 0;
#endif
}

ScopedRenderThreadTiming::~ScopedRenderThreadTiming() {
#ifdef _WIN32
    if (active_) {
        timeEndPeriod(1);
    }
#endif
}

std::string current_render_thread_id_string() {
#ifdef _WIN32
    return std::to_string(GetCurrentThreadId());
#else
    std::ostringstream stream;
    stream << std::this_thread::get_id();
    return stream.str();
#endif
}

} // namespace vr
