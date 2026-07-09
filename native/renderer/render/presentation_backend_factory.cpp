#include "renderer/render/presentation_backend_factory.h"

#ifdef __APPLE__
#include "macos/metal/metal_presentation_backend.h"
#endif

namespace vr {
namespace {

class DefaultPresentationBackendProvider final : public PresentationBackendProvider {
public:
    bool supports(RenderBackendKind kind) const override {
#ifdef _WIN32
        (void)kind;
        return false;
#elif defined(__APPLE__)
        return kind == RenderBackendKind::Metal;
#else
        (void)kind;
        return false;
#endif
    }

    std::unique_ptr<PresentationBackend> create(RenderBackendKind kind) const override {
#ifdef _WIN32
        (void)kind;
#elif defined(__APPLE__)
        if (kind == RenderBackendKind::Metal) {
            return vp_macos::create_metal_presentation_backend();
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
