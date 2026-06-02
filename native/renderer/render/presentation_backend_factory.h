#pragma once

#include "renderer/render/backend_type.h"
#include "renderer/render/presentation_backend.h"

#include <memory>

namespace vr {

class PresentationBackendProvider {
public:
    virtual ~PresentationBackendProvider() = default;

    virtual bool supports(RenderBackendKind kind) const = 0;
    virtual std::unique_ptr<PresentationBackend> create(RenderBackendKind kind) const = 0;
};

const PresentationBackendProvider* default_presentation_backend_provider();

std::unique_ptr<PresentationBackend> create_presentation_backend(
    RenderBackendKind kind);

} // namespace vr
