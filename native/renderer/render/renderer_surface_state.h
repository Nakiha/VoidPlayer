#pragma once

#include "renderer/render/backend_type.h"
#include "renderer/renderer_config.h"

namespace vr {

struct RendererSurfaceResize {
    bool changed = false;
    int old_width = 0;
    int old_height = 0;
    int width = 0;
    int height = 0;
};

// Lock contract:
// - Owns renderer surface configuration: host window, headless flag, target
//   dimensions, and selected render backend kind.
// - Does not take locks, call callbacks, or touch presentation resources.
// - Callers serialize mutations with the renderer state/lifecycle locks.
class RendererSurfaceState {
public:
    void configure(const RendererConfig& config);
    void reset();

    void* hwnd() const;
    bool headless() const;
    int width() const;
    int height() const;
    RenderBackendKind backend_kind() const;

    RendererSurfaceResize resize_if_changed(int width, int height);

private:
    void* hwnd_ = nullptr;
    bool headless_ = false;
    int width_ = 1920;
    int height_ = 1080;
    RenderBackendKind backend_kind_ = RenderBackendKind::D3D11;
};

} // namespace vr
