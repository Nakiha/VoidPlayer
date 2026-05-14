#include "video_renderer/track_snapshot.h"

#include <utility>

namespace vr {

std::vector<TrackInfo> snapshot_track_infos(const TrackPipelineManager& tracks) {
    std::vector<TrackInfo> infos;
    for (size_t i = 0; i < kMaxTracks; ++i) {
        if (!tracks[i]) {
            continue;
        }

        const auto& track = *tracks[i];
        const auto* demux = track.demux_thread.get();
        const auto* decode = track.decode_thread.get();
        TrackInfo info;
        info.file_id = track.file_id;
        info.slot = static_cast<int>(i);
        info.file_path = track.file_path;
        info.width = track.video_width;
        info.height = track.video_height;
        if (demux) {
            const auto& stats = demux->stats();
            info.duration_us = stats.duration_us;
            info.start_time_us = stats.start_time_us;
            info.bit_rate = stats.bit_rate;
            info.format_name = stats.format_name;
            info.codec_name = stats.codec_name;
            info.codec_long_name = stats.codec_long_name;
        }
        if (decode) {
            info.decoder_name = decode->decoder_name();
        }
        infos.push_back(std::move(info));
    }
    return infos;
}

} // namespace vr
