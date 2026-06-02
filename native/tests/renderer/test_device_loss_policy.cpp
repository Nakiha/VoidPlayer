#include <catch2/catch_test_macros.hpp>

#include "renderer/render/device_loss_policy.h"

using namespace vr;

TEST_CASE("DeviceLossPolicy: device loss enters terminal state once",
          "[renderer][device_loss_policy]") {
    const auto ready_plan =
        plan_renderer_device_lost_transition(RendererDeviceState::Ready);
    REQUIRE(ready_plan.apply);
    REQUIRE(ready_plan.count_device_lost);
    REQUIRE(ready_plan.pause_playback);
    REQUIRE(ready_plan.pause_decode);
    REQUIRE_FALSE(ready_plan.clear_initialized);
    REQUIRE(ready_plan.pre_terminal_state == RendererDeviceState::Lost);
    REQUIRE(ready_plan.final_state == RendererDeviceState::Terminal);

    const auto lost_plan =
        plan_renderer_device_lost_transition(RendererDeviceState::Lost);
    REQUIRE(lost_plan.apply);
    REQUIRE(lost_plan.count_device_lost);

    const auto terminal_plan =
        plan_renderer_device_lost_transition(RendererDeviceState::Terminal);
    REQUIRE_FALSE(terminal_plan.apply);
    REQUIRE_FALSE(terminal_plan.count_device_lost);
}

TEST_CASE("DeviceLossPolicy: runtime error terminalizes without device-loss metrics",
          "[renderer][device_loss_policy]") {
    const auto ready_plan =
        plan_renderer_runtime_error_transition(RendererDeviceState::Ready);
    REQUIRE(ready_plan.apply);
    REQUIRE_FALSE(ready_plan.count_device_lost);
    REQUIRE(ready_plan.pause_playback);
    REQUIRE(ready_plan.pause_decode);
    REQUIRE(ready_plan.clear_initialized);
    REQUIRE(ready_plan.pre_terminal_state == RendererDeviceState::Terminal);
    REQUIRE(ready_plan.final_state == RendererDeviceState::Terminal);

    const auto lost_plan =
        plan_renderer_runtime_error_transition(RendererDeviceState::Lost);
    REQUIRE(lost_plan.apply);
    REQUIRE_FALSE(lost_plan.count_device_lost);

    const auto terminal_plan =
        plan_renderer_runtime_error_transition(RendererDeviceState::Terminal);
    REQUIRE_FALSE(terminal_plan.apply);
}
