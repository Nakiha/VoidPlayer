#pragma once

#include "media/demux_thread.h"
#include "media/seek_controller.h"

#include <cstdint>

namespace vr {

class PacketQueue;
class PlaybackController;

class AudioCoordinator {
public:
    explicit AudioCoordinator(PlaybackController& playback);

    bool available() const;
    bool register_track(int file_id, PacketQueue& audio_packet_queue, const DemuxStats& stats);
    void unregister_track(int file_id);
    void set_active_track(int file_id);
    int active_track() const;
    void set_track_decode_paused(int file_id, bool paused);
    void set_all_decode_paused(bool paused);
    void notify_seek(int file_id, int64_t target_pts_us, SeekType type);

private:
    PlaybackController& playback_;
};

} // namespace vr
