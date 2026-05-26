#pragma once

#include "audio/audio_output.h"
#include <cstdint>
#include <memory>

namespace vr {

class AudioEngine final : public AudioOutput {
public:
    AudioEngine();
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool add_track(int file_id,
                   PacketQueue& input_queue,
                   const AVCodecParameters* codec_params,
                   AVRational time_base) override;
    void remove_track(int file_id) override;
    void clear() override;

    void play() override;
    void pause() override;
    void set_active_track(int file_id) override;
    int active_track() const override;

    void set_track_decode_paused(int file_id, bool paused) override;
    void set_all_decode_paused(bool paused) override;
    void notify_seek(int file_id, int64_t target_pts_us, SeekType type) override;
    AudioOutputStats stats() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vr
