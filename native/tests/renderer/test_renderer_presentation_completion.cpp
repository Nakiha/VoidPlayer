#include "renderer/render/renderer_presentation_completion.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Renderer presentation completion treats stale async source drops as nonfatal",
          "[renderer][presentation]") {
    vr::RendererPresentationCompletionInput input;
    input.attempted_draw = true;
    input.drew = false;
    input.frame_callback_available = true;
    input.frame_failure_callback_available = true;
    input.frame_failure_error =
        "renderer-owned wgpu-metal stale async draw dropped";

    const auto decision = vr::plan_presentation_completion(input);

    REQUIRE(decision.callback_available);
    REQUIRE_FALSE(decision.callback_published);
    REQUIRE_FALSE(decision.transient_backpressure);
    REQUIRE_FALSE(decision.notify_frame_failure);
}

TEST_CASE("Renderer presentation completion treats stale output drops as nonfatal",
          "[renderer][presentation]") {
    vr::RendererPresentationCompletionInput input;
    input.attempted_draw = true;
    input.drew = false;
    input.frame_callback_available = true;
    input.frame_failure_callback_available = true;
    input.frame_failure_error =
        "renderer-owned wgpu-metal stale output draw dropped";

    const auto decision = vr::plan_presentation_completion(input);

    REQUIRE(decision.callback_available);
    REQUIRE_FALSE(decision.callback_published);
    REQUIRE_FALSE(decision.transient_backpressure);
    REQUIRE_FALSE(decision.notify_frame_failure);
}
