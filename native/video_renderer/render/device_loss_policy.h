#pragma once

#include "video_renderer/render/renderer_device_state.h"

namespace vr {

struct RendererTerminalTransitionPlan {
    bool apply = false;
    bool count_device_lost = false;
    bool pause_playback = false;
    bool pause_decode = false;
    bool clear_initialized = false;
    RendererDeviceState pre_terminal_state = RendererDeviceState::Ready;
    RendererDeviceState final_state = RendererDeviceState::Ready;
};

inline RendererTerminalTransitionPlan plan_renderer_device_lost_transition(
    RendererDeviceState current_state) {
    if (current_state == RendererDeviceState::Terminal) {
        return {};
    }
    RendererTerminalTransitionPlan plan;
    plan.apply = true;
    plan.count_device_lost = true;
    plan.pause_playback = true;
    plan.pause_decode = true;
    plan.pre_terminal_state = RendererDeviceState::Lost;
    plan.final_state = RendererDeviceState::Terminal;
    return plan;
}

inline RendererTerminalTransitionPlan plan_renderer_runtime_error_transition(
    RendererDeviceState current_state) {
    if (current_state == RendererDeviceState::Terminal) {
        return {};
    }
    RendererTerminalTransitionPlan plan;
    plan.apply = true;
    plan.pause_playback = true;
    plan.pause_decode = true;
    plan.clear_initialized = true;
    plan.pre_terminal_state = RendererDeviceState::Terminal;
    plan.final_state = RendererDeviceState::Terminal;
    return plan;
}

} // namespace vr
