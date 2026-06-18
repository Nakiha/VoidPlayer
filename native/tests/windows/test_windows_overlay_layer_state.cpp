#include "windows/presentation/windows_overlay_layer_state.h"

#include <catch2/catch_test_macros.hpp>

namespace {

vr::WindowsOverlayLayerSignature signature(uint64_t generation) {
    vr::WindowsOverlayLayerSignature value;
    value.primitive_generation = generation;
    value.track_signature = 42;
    value.target_class = 1;
    value.sdr_white_scale_x1000 = 1000;
    value.source_width = 1920;
    value.source_height = 1080;
    value.fill_rect_count = 4;
    value.outline_rect_count = 8;
    value.motion_line_count = 2;
    return value;
}

} // namespace

TEST_CASE("Windows overlay layer state reuses stable signature",
          "[windows_overlay_layer]") {
    vr::WindowsOverlayLayerCacheState state;
    REQUIRE(state.prepare(signature(7), 4096));
    REQUIRE_FALSE(state.prepare(signature(7), 4096));
    state.composite();

    const auto snapshot = state.snapshot();
    REQUIRE(snapshot.active);
    REQUIRE(snapshot.mode == "retained-primitive-buffer");
    REQUIRE(snapshot.generation == 7);
    REQUIRE(snapshot.committed_generation == 7);
    REQUIRE(snapshot.raster_count == 1);
    REQUIRE(snapshot.upload_count == 1);
    REQUIRE(snapshot.reuse_count == 1);
    REQUIRE(snapshot.composite_count == 1);
    REQUIRE(snapshot.fallback_reason == "none");
}

TEST_CASE("Windows overlay layer state rebuilds on dirty signature",
          "[windows_overlay_layer]") {
    vr::WindowsOverlayLayerCacheState state;
    REQUIRE(state.prepare(signature(7), 4096));
    REQUIRE(state.prepare(signature(8), 8192));

    const auto snapshot = state.snapshot();
    REQUIRE(snapshot.generation == 8);
    REQUIRE(snapshot.bytes == 8192);
    REQUIRE(snapshot.raster_count == 2);
    REQUIRE(snapshot.upload_count == 2);
    REQUIRE(snapshot.reuse_count == 0);
}

TEST_CASE("Windows overlay layer state rejects invalid generation",
          "[windows_overlay_layer]") {
    vr::WindowsOverlayLayerCacheState state;
    auto invalid = signature(0);
    REQUIRE_FALSE(state.prepare(invalid, 0));

    const auto snapshot = state.snapshot();
    REQUIRE_FALSE(snapshot.active);
    REQUIRE(snapshot.miss_count == 1);
    REQUIRE(snapshot.last_error == "invalid-signature");
}

TEST_CASE("Windows overlay layer state records fallback and reset",
          "[windows_overlay_layer]") {
    vr::WindowsOverlayLayerCacheState state;
    REQUIRE(state.prepare(signature(1), 128));
    state.fail("device-removed");
    REQUIRE_FALSE(state.snapshot().active);
    REQUIRE(state.snapshot().fallback_reason == "device-removed");

    state.reset("source-cache-clear");
    REQUIRE_FALSE(state.snapshot().active);
    REQUIRE(state.snapshot().fallback_reason == "source-cache-clear");
    REQUIRE(state.snapshot().last_error == "none");
}
