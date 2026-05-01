#pragma once

#include "audio/audio_engine.h"
#include "video_renderer/clock.h"
#include <cstdint>
#include <memory>

namespace vr {

/// Owns playback-level state shared by video and audio.
///
/// Renderer remains the video sink/facade for now, but clock and audio output
/// lifecycle live here so play/pause/seek controls have a single native owner.
class PlaybackController {
public:
    PlaybackController();
    ~PlaybackController();

    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    void start_session();
    void stop_session();

    Clock& clock() { return clock_; }
    const Clock& clock() const { return clock_; }

    AudioEngine* audio_engine() { return audio_engine_.get(); }
    const AudioEngine* audio_engine() const { return audio_engine_.get(); }

    void play();
    void pause();
    void seek_clock(int64_t target_pts_us);
    void set_speed(double speed);
    double speed() const;

private:
    Clock clock_;
    std::unique_ptr<AudioEngine> audio_engine_;
};

} // namespace vr
