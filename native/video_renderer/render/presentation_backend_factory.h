#pragma once

#include "video_renderer/render/backend_type.h"
#include "video_renderer/render/presentation_backend.h"

#include <memory>

namespace vr {

std::unique_ptr<PresentationBackend> create_presentation_backend(
    RenderBackendKind kind);

} // namespace vr
