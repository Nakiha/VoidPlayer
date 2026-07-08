#include <catch2/catch_test_macros.hpp>

#include "renderer/render/presentation_color_domain_contract.h"

TEST_CASE("Flutter SDR native shader into HDR target is a color contract violation",
          "[presentation_color_domain_contract]") {
    vr::PresentationBackendDiagnostics diagnostics;
    diagnostics.external_flutter_surface_color_domain =
        "sdr-srgb-premultiplied-bgra8";
    diagnostics.external_flutter_surface_composition_owner = "native-shader";
    diagnostics.external_flutter_surface_target_domain =
        "windows-linear-scrgb";
    diagnostics.external_flutter_surface_composited_into_hdr_target = true;

    const auto contract =
        vr::evaluate_flutter_surface_color_contract(diagnostics);

    REQUIRE(contract.violation);
    REQUIRE(contract.reason == "sdr-flutter-native-shader-hdr-target");
}

TEST_CASE("System managed SDR Flutter surface keeps color contract valid",
          "[presentation_color_domain_contract]") {
    vr::PresentationBackendDiagnostics diagnostics;
    diagnostics.external_flutter_surface_color_domain =
        "sdr-srgb-premultiplied-bgra8";
    diagnostics.external_flutter_surface_composition_owner = "system-managed";
    diagnostics.external_flutter_surface_target_domain =
        "windows-linear-scrgb";
    diagnostics.external_flutter_surface_composited_into_hdr_target = true;

    const auto contract =
        vr::evaluate_flutter_surface_color_contract(diagnostics);

    REQUIRE_FALSE(contract.violation);
    REQUIRE(contract.reason == "none");
}

TEST_CASE("Native shader SDR target keeps Flutter color contract valid",
          "[presentation_color_domain_contract]") {
    vr::PresentationBackendDiagnostics diagnostics;
    diagnostics.external_flutter_surface_color_domain =
        "sdr-srgb-premultiplied-bgra8";
    diagnostics.external_flutter_surface_composition_owner = "native-shader";
    diagnostics.external_flutter_surface_target_domain = "sdr-bgra8";

    const auto contract =
        vr::evaluate_flutter_surface_color_contract(diagnostics);

    REQUIRE_FALSE(contract.violation);
}
