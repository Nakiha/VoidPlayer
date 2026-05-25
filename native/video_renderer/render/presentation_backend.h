#pragma once

#include "video_renderer/render/backend_type.h"
#include "video_renderer/render/renderer_draw_snapshot.h"

#include <cstdint>
#include <functional>

namespace vr {

using PresentationBackendKind = RenderBackendKind;
class PresentationBackend;

struct PresentationBackendConfig {
    void* hwnd = nullptr;
    void* adapter = nullptr;
    int width = 0;
    int height = 0;
    bool headless = false;
};

struct PresentationBackendDrawHooks {
    std::function<void(const char*)> wait_gpu_idle;
    std::function<void(uint64_t)> record_frame_copy_us;
    std::function<void(PresentationBackend&, const RendererDrawSnapshot&)> draw_overlay;
};

class PresentationBackend {
public:
    virtual ~PresentationBackend() = default;

    virtual PresentationBackendKind kind() const = 0;
    virtual const char* name() const = 0;
    virtual bool initialize(const PresentationBackendConfig& config) = 0;
    virtual void shutdown() = 0;
    virtual bool headless() const = 0;
    virtual bool draw_frame(const RendererDrawSnapshot& snapshot,
                            const PresentationBackendDrawHooks& hooks) = 0;
};

} // namespace vr
