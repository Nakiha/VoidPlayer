#include "renderer/render/presentation_backend_factory.h"

#ifdef _WIN32
#include "windows/wgpu/d3d12_presentation_backend.h"
#endif

#ifdef __APPLE__
#include "macos/wgpu/wgpu_metal_presentation_backend.h"
#endif

namespace vr {
namespace {

class DefaultPresentationBackendProvider final : public PresentationBackendProvider {
public:
    bool supports(RenderBackendKind kind) const override {
#ifdef _WIN32
        return kind == RenderBackendKind::WgpuD3D12;
#elif defined(__APPLE__)
        return kind == RenderBackendKind::WgpuMetal;
#else
        (void)kind;
        return false;
#endif
    }

    std::unique_ptr<PresentationBackend> create(RenderBackendKind kind) const override {
#ifdef _WIN32
        if (kind == RenderBackendKind::WgpuD3D12) {
            return std::make_unique<WgpuD3D12PresentationBackend>();
        }
#elif defined(__APPLE__)
        if (kind == RenderBackendKind::WgpuMetal) {
            return std::make_unique<vp_macos::WgpuMetalPresentationBackend>();
        }
#else
        (void)kind;
#endif
        return nullptr;
    }
};

}  // namespace

const PresentationBackendProvider* default_presentation_backend_provider() {
    static const DefaultPresentationBackendProvider provider;
    return &provider;
}

std::unique_ptr<PresentationBackend> create_presentation_backend(
    RenderBackendKind kind) {
    const auto* provider = default_presentation_backend_provider();
    return provider && provider->supports(kind) ? provider->create(kind) : nullptr;
}

} // namespace vr
