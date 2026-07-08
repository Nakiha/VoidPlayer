#include "renderer/render/presentation_color_domain_contract.h"

namespace vr {
namespace {

bool starts_with(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool contains(const std::string& value, const char* needle) {
    return value.find(needle) != std::string::npos;
}

} // namespace

PresentationFlutterSurfaceColorContract
evaluate_flutter_surface_color_contract(
    const PresentationBackendDiagnostics& diagnostics) {
    PresentationFlutterSurfaceColorContract contract;
    if (diagnostics.external_flutter_surface_color_domain == "none" ||
        diagnostics.external_flutter_surface_composition_owner == "none") {
        return contract;
    }
    const bool flutter_is_sdr =
        starts_with(diagnostics.external_flutter_surface_color_domain, "sdr");
    const bool native_shader_owner =
        diagnostics.external_flutter_surface_composition_owner ==
        "native-shader";
    const bool hdr_target =
        diagnostics.external_flutter_surface_composited_into_hdr_target ||
        contains(diagnostics.external_flutter_surface_target_domain, "scrgb") ||
        contains(diagnostics.external_flutter_surface_target_domain, "hdr") ||
        contains(diagnostics.external_flutter_surface_target_domain, "edr");
    if (flutter_is_sdr && native_shader_owner && hdr_target) {
        contract.violation = true;
        contract.reason = "sdr-flutter-native-shader-hdr-target";
    }
    return contract;
}

} // namespace vr
