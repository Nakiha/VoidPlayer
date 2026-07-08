#pragma once

#include "renderer/render/presentation_backend_types.h"

#include <string>

namespace vr {

struct PresentationFlutterSurfaceColorContract {
    bool violation = false;
    std::string reason = "none";
};

PresentationFlutterSurfaceColorContract
evaluate_flutter_surface_color_contract(
    const PresentationBackendDiagnostics& diagnostics);

} // namespace vr
