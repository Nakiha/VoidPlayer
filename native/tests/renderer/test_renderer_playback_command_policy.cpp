#include <catch2/catch_test_macros.hpp>

#include "renderer/playback/renderer_playback_command_policy.h"

using namespace vr;

TEST_CASE("RendererPlaybackCommandPolicy: play command is gated by init and current play state",
          "[renderer][playback_command_policy]") {
    REQUIRE_FALSE(plan_renderer_play_command(false, false).execute);
    REQUIRE_FALSE(plan_renderer_play_command(true, true).execute);

    const auto plan = plan_renderer_play_command(true, false);
    REQUIRE(plan.execute);
    REQUIRE(plan.reset_seek);
    REQUIRE(plan.playback_active);
    REQUIRE(plan.play_clock);
    REQUIRE_FALSE(plan.pause_clock);
    REQUIRE(plan.playing);
}

TEST_CASE("RendererPlaybackCommandPolicy: pause command always pauses playback",
          "[renderer][playback_command_policy]") {
    const auto plan = plan_renderer_pause_command();
    REQUIRE(plan.execute);
    REQUIRE_FALSE(plan.reset_seek);
    REQUIRE_FALSE(plan.playback_active);
    REQUIRE_FALSE(plan.play_clock);
    REQUIRE(plan.pause_clock);
    REQUIRE_FALSE(plan.playing);
}

TEST_CASE("RendererPlaybackCommandPolicy: step command pauses only when ready",
          "[renderer][playback_command_policy]") {
    REQUIRE_FALSE(plan_renderer_step_command(false, false).execute);
    REQUIRE_FALSE(plan_renderer_step_command(true, true).execute);

    const auto plan = plan_renderer_step_command(true, false);
    REQUIRE(plan.execute);
    REQUIRE(plan.pause_clock);
    REQUIRE_FALSE(plan.playing);
}
