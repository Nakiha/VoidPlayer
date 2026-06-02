#include "renderer/playback/renderer_playback_command_policy.h"

namespace vr {

RendererPlaybackCommandPlan plan_renderer_play_command(bool initialized,
                                                       bool already_playing) {
    if (!initialized || already_playing) {
        return {};
    }
    return RendererPlaybackCommandPlan{
        true,
        true,
        true,
        true,
        false,
        true,
    };
}

RendererPlaybackCommandPlan plan_renderer_pause_command() {
    return RendererPlaybackCommandPlan{
        true,
        false,
        false,
        false,
        true,
        false,
    };
}

RendererStepCommandPlan plan_renderer_step_command(bool initialized,
                                                   bool has_buffering_track) {
    if (!initialized || has_buffering_track) {
        return {};
    }
    return RendererStepCommandPlan{
        true,
        true,
        false,
    };
}

} // namespace vr
