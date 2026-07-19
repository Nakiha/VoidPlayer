#pragma once

#include "media/media_input_session.h"
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avio.h>
}

namespace vr {

class PrivateCdnFlvDemuxer {
public:
    PrivateCdnFlvDemuxer() = default;
    ~PrivateCdnFlvDemuxer();

    PrivateCdnFlvDemuxer(const PrivateCdnFlvDemuxer&) = delete;
    PrivateCdnFlvDemuxer& operator=(const PrivateCdnFlvDemuxer&) = delete;

    static bool probe(const std::string& path);

    bool open(const std::string& path);
    void close();

    int read_packet(AVPacket* pkt);
    int seek(int stream_index, int64_t timestamp_ms);
    void flush();

    const DemuxStats& stats() const { return stats_; }
    AVRational time_base_for_stream(int stream_index) const;

private:
    struct PacketRecord {
        int stream_index = -1;
        int64_t tag_offset = 0;
        int64_t payload_offset = 0;
        int payload_size = 0;
        int64_t dts_ms = 0;
        int64_t pts_ms = 0;
        int64_t duration_ms = 0;
        bool keyframe = false;
    };

    bool scan();
    void reset_owned_params();
    bool copy_extradata(AVCodecParameters* par, int64_t offset, int size);
    void ensure_video_params(AVCodecID codec_id);
    void ensure_aac_params();
    void ensure_mp3_params(int sample_rate, int channels);
    void finalize_packet_durations();

    AVIOContext* pb_ = nullptr;
    DemuxStats stats_;
    AVCodecParameters* video_params_ = nullptr;
    AVCodecParameters* audio_params_ = nullptr;
    std::vector<PacketRecord> packets_;
    size_t packet_cursor_ = 0;
};

} // namespace vr
