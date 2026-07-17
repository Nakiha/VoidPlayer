#include <catch2/catch_test_macros.hpp>

#include "renderer/render/renderer_presentation_completion.h"

using namespace vr;

TEST_CASE("Renderer presentation completion recycles stale offscreen targets",
          "[renderer][presentation_completion]") {
    PresentationBackendFrameInfo frame_info;
    frame_info.target_pixel_buffer_address = 0x1234;

    const auto decision = plan_presentation_completion({
        false,
        true,
        true,
        true,
        true,
        false,
        nullptr,
        42,
        &frame_info,
    });

    REQUIRE_FALSE(decision.callback_published);
    REQUIRE(decision.release_discarded_target);
    REQUIRE(decision.discarded_target_address == 0x1234);
}

TEST_CASE("Renderer presentation completion leaves published targets to host",
          "[renderer][presentation_completion]") {
    PresentationBackendFrameInfo frame_info;
    frame_info.target_pixel_buffer_address = 0x5678;

    const auto decision = plan_presentation_completion({
        false,
        true,
        true,
        false,
        true,
        false,
        nullptr,
        43,
        &frame_info,
    });

    REQUIRE(decision.callback_published);
    REQUIRE_FALSE(decision.release_discarded_target);
    REQUIRE(decision.discarded_target_address == 0);
}
