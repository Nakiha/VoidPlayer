#include "renderer/render/renderer_surface_state.h"

namespace vr {

void RendererSurfaceState::configure(const RendererConfig& config) {
    hwnd_ = config.hwnd;
    headless_ = config.headless;
    width_ = config.width;
    height_ = config.height;
    backend_kind_ = config.backend.type;
}

void RendererSurfaceState::reset() {
    hwnd_ = nullptr;
    headless_ = false;
    width_ = 1920;
    height_ = 1080;
    backend_kind_ = default_render_backend_kind();
}

void* RendererSurfaceState::hwnd() const {
    return hwnd_;
}

bool RendererSurfaceState::headless() const {
    return headless_;
}

int RendererSurfaceState::width() const {
    return width_;
}

int RendererSurfaceState::height() const {
    return height_;
}

RenderBackendKind RendererSurfaceState::backend_kind() const {
    return backend_kind_;
}

RendererSurfaceResize RendererSurfaceState::resize_if_changed(
    int width,
    int height) {
    RendererSurfaceResize result;
    result.old_width = width_;
    result.old_height = height_;
    result.width = width;
    result.height = height;
    if (width == width_ && height == height_) {
        return result;
    }
    width_ = width;
    height_ = height;
    result.changed = true;
    return result;
}

} // namespace vr
