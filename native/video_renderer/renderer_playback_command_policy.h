#pragma once

namespace vr {

struct RendererPlaybackCommandPlan {
    bool execute = false;
    bool reset_seek = false;
    bool playback_active = false;
    bool play_clock = false;
    bool pause_clock = false;
    bool playing = false;
};

struct RendererStepCommandPlan {
    bool execute = false;
    bool pause_clock = false;
    bool playing = false;
};

RendererPlaybackCommandPlan plan_renderer_play_command(bool initialized,
                                                       bool already_playing);
RendererPlaybackCommandPlan plan_renderer_pause_command();
RendererStepCommandPlan plan_renderer_step_command(bool initialized,
                                                   bool has_buffering_track);

} // namespace vr
