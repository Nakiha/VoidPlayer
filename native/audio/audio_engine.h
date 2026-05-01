#pragma once

#include "media/packet_queue.h"
#include "media/seek_controller.h"
#include <cstdint>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

namespace vr {

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool add_track(int file_id,
                   PacketQueue& input_queue,
                   const AVCodecParameters* codec_params,
                   AVRational time_base);
    void remove_track(int file_id);
    void clear();

    void play();
    void pause();
    void set_active_track(int file_id);
    int active_track() const;

    void set_track_decode_paused(int file_id, bool paused);
    void set_all_decode_paused(bool paused);
    void notify_seek(int file_id, int64_t target_pts_us, SeekType type);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vr
