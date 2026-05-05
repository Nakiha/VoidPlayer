#pragma once

#include "audio/audio_output.h"
#include "video_renderer/clock.h"
#include <cstdint>
#include <functional>
#include <memory>

namespace vr {

/// Owns playback-level state shared by video and audio.
///
/// Renderer remains the video sink/facade for now, but clock and audio output
/// lifecycle live here so play/pause/seek controls have a single native owner.
class PlaybackController {
public:
    using AudioOutputFactory = std::function<std::unique_ptr<AudioOutput>()>;

    PlaybackController();
    explicit PlaybackController(AudioOutputFactory audio_output_factory);
    ~PlaybackController();

    PlaybackController(const PlaybackController&) = delete;
    PlaybackController& operator=(const PlaybackController&) = delete;

    void start_session();
    void stop_session();

    Clock& clock() { return clock_; }
    const Clock& clock() const { return clock_; }

    AudioOutput* audio_output() { return audio_output_.get(); }
    const AudioOutput* audio_output() const { return audio_output_.get(); }

    void play();
    void pause();
    void seek_clock(int64_t target_pts_us);
    void set_speed(double speed);
    double speed() const;

private:
    Clock clock_;
    AudioOutputFactory audio_output_factory_;
    std::unique_ptr<AudioOutput> audio_output_;
};

} // namespace vr
