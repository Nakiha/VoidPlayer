#include "renderer/render/presentation_backend_factory.h"

#if !defined(_WIN32) && !defined(__APPLE__)

namespace vr {
namespace {

class NullPresentationBackendProvider final : public PresentationBackendProvider {
public:
    bool supports(RenderBackendKind kind) const override {
        (void)kind;
        return false;
    }

    std::unique_ptr<PresentationBackend> create(RenderBackendKind kind) const override {
        (void)kind;
        return nullptr;
    }
};

}  // namespace

const PresentationBackendProvider* default_presentation_backend_provider() {
    static const NullPresentationBackendProvider provider;
    return &provider;
}

std::unique_ptr<PresentationBackend> create_presentation_backend(
    RenderBackendKind kind) {
    const auto* provider = default_presentation_backend_provider();
    return provider && provider->supports(kind) ? provider->create(kind) : nullptr;
}

}  // namespace vr

#endif
