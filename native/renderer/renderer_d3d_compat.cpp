#include "renderer/renderer_internal.h"

namespace vr {

D3D11RenderBackend* Renderer::d3d_backend() const {
#ifdef _WIN32
    if (!presentation_backend_ ||
        presentation_backend_->kind() != PresentationBackendKind::D3D11) {
        return nullptr;
    }
    return static_cast<D3D11RenderBackend*>(presentation_backend_.get());
#else
    return nullptr;
#endif
}

D3D11Device* Renderer::d3d_device() const {
#ifdef _WIN32
    auto* backend = d3d_backend();
    return backend ? backend->device() : nullptr;
#else
    return nullptr;
#endif
}

D3D11FramePresenter* Renderer::frame_presenter() const {
#ifdef _WIN32
    auto* backend = d3d_backend();
    return backend ? backend->frame_presenter() : nullptr;
#else
    return nullptr;
#endif
}

D3D11HeadlessOutput* Renderer::headless_output() const {
#ifdef _WIN32
    auto* backend = d3d_backend();
    return backend ? backend->headless_output() : nullptr;
#else
    return nullptr;
#endif
}

D3D11RenderResources* Renderer::d3d_resources() const {
#ifdef _WIN32
    auto* backend = d3d_backend();
    return backend ? backend->resources() : nullptr;
#else
    return nullptr;
#endif
}

} // namespace vr
