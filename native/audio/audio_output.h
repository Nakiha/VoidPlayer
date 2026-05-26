#pragma once

#include "media/packet_queue.h"
#include "media/seek_controller.h"
#include <cstddef>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
}

namespace vr {

struct AudioOutputStats {
    bool device_initialized = false;
    bool playing = false;
    int active_track = -1;
    int output_sample_rate = 0;
    int output_channels = 0;
    size_t registered_track_count = 0;
    bool active_track_registered = false;
    size_t active_track_queued_frames = 0;
    int64_t active_track_queued_duration_us = 0;
    size_t active_track_underrun_frames = 0;
    size_t active_track_discarded_frames = 0;
    size_t active_track_seek_trimmed_frames = 0;
};

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    virtual bool add_track(int file_id,
                           PacketQueue& input_queue,
                           const AVCodecParameters* codec_params,
                           AVRational time_base) = 0;
    virtual void remove_track(int file_id) = 0;
    virtual void clear() = 0;

    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void set_active_track(int file_id) = 0;
    virtual int active_track() const = 0;

    virtual void set_track_decode_paused(int file_id, bool paused) = 0;
    virtual void set_all_decode_paused(bool paused) = 0;
    virtual void notify_seek(int file_id, int64_t target_pts_us, SeekType type) = 0;
    virtual AudioOutputStats stats() const = 0;
};

} // namespace vr
