#include <catch2/catch_test_macros.hpp>

#include "video_renderer/render/swap_chain_present_policy.h"
#include "video_renderer/renderer.h"

using namespace vr;

TEST_CASE("Renderer: shutdown clears stored event callback",
          "[renderer][lifecycle]") {
    Renderer renderer;
    renderer.set_event_callback([](const RendererEvent&) {});

    REQUIRE(renderer.has_event_callback_for_test());

    renderer.shutdown();

    REQUIRE_FALSE(renderer.has_event_callback_for_test());
}

TEST_CASE("Renderer: render-loop exception test seam enters terminal state",
          "[renderer][lifecycle]") {
    Renderer renderer;

    REQUIRE(renderer.device_state() == RendererDeviceState::Ready);

    renderer.enter_terminal_render_loop_error_for_test("test fault");

    REQUIRE(renderer.device_state() == RendererDeviceState::Terminal);
    REQUIRE_FALSE(renderer.is_initialized());
    REQUIRE_FALSE(renderer.is_playing());
}

TEST_CASE("SwapChainPresentPolicy: draw failure skips swap-chain present",
          "[renderer][present_policy]") {
    REQUIRE(should_present_swap_chain_after_draw(true, true));
    REQUIRE_FALSE(should_present_swap_chain_after_draw(false, true));
    REQUIRE_FALSE(should_present_swap_chain_after_draw(true, false));
    REQUIRE_FALSE(should_present_swap_chain_after_draw(false, false));
}
