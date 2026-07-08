#include <catch2/catch_test_macros.hpp>

#include "renderer/render/presentation_source_cache_state.h"

TEST_CASE("Presentation source cache ready state merges backend and compositor",
          "[presentation_source_cache_state]") {
    vr::PresentationBackendDiagnostics backend;
    backend.source_cache_active = true;
    backend.source_cache_texture_count = 2;
    backend.source_cache_generation = 9;
    backend.source_cache_publish_count = 4;
    backend.source_cache_backpressure_count = 3;
    backend.source_cache_fallback_count = 1;
    backend.source_cache_required_mask = 3;
    backend.source_cache_drawn_mask = 1;
    backend.source_cache_missing_mask = 2;
    backend.source_cache_incomplete_publish_suppressed_count = 5;
    backend.source_cache_last_error = "backend-error";
    backend.source_cache_frozen_snapshot = true;
    vr::PresentationSourceCacheCompositorState compositor;
    compositor.accepts_source_cache = true;
    compositor.consumed_generation = 7;
    compositor.fallback_count = 2;
    compositor.last_error = "none";

    const auto state =
        vr::build_presentation_source_cache_ready_state(backend, compositor);

    REQUIRE(state.active);
    REQUIRE(state.ready);
    REQUIRE(state.frozen_snapshot);
    REQUIRE(state.texture_count == 2);
    REQUIRE(state.generation == 9);
    REQUIRE(state.publish_count == 4);
    REQUIRE(state.consumed_generation == 7);
    REQUIRE(state.backpressure_count == 3);
    REQUIRE(state.fallback_count == 3);
    REQUIRE(state.required_mask == 3);
    REQUIRE(state.drawn_mask == 1);
    REQUIRE(state.missing_mask == 2);
    REQUIRE(state.incomplete_publish_suppressed_count == 5);
    REQUIRE(state.last_error == "backend-error");
}

TEST_CASE("Presentation source cache ready state honors compositor failures",
          "[presentation_source_cache_state]") {
    vr::PresentationBackendDiagnostics backend;
    backend.source_cache_texture_count = 2;
    backend.source_cache_publish_count = 4;
    backend.source_cache_last_error = "backend-error";
    vr::PresentationSourceCacheCompositorState compositor;
    compositor.accepts_source_cache = false;
    compositor.last_error = "compositor-stopped";

    const auto state =
        vr::build_presentation_source_cache_ready_state(backend, compositor);

    REQUIRE_FALSE(state.active);
    REQUIRE_FALSE(state.ready);
    REQUIRE(state.last_error == "compositor-stopped");
}

TEST_CASE("Presentation source cache commit state tracks missing slots",
          "[presentation_source_cache_state]") {
    auto state = vr::build_presentation_source_cache_commit_state(
        3, uint64_t{0b101});

    REQUIRE_FALSE(state.complete);
    REQUIRE(state.required_mask == uint64_t{0b111});
    REQUIRE(state.drawn_mask == uint64_t{0b101});
    REQUIRE(state.missing_mask == uint64_t{0b010});
    REQUIRE(state.error == "source-package-incomplete");

    state = vr::build_presentation_source_cache_commit_state(
        3, uint64_t{0b111});
    REQUIRE(state.complete);
    REQUIRE(state.missing_mask == 0);
    REQUIRE(state.error == "none");
}
