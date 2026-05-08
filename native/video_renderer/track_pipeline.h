#pragma once

#include "media/demux_thread.h"
#include "media/packet_queue.h"
#include "media/seek_controller.h"
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/decode/decode_thread.h"
#include "video_renderer/sync/render_sink.h"

#include <array>
#include <functional>
#include <memory>
#include <string>

namespace vr {

struct TrackPipeline {
    int file_id = 0;
    std::string file_path;
    int64_t offset_us = 0;
    std::unique_ptr<PacketQueue> packet_queue;
    std::unique_ptr<PacketQueue> audio_packet_queue;
    std::unique_ptr<TrackBuffer> track_buffer;
    std::unique_ptr<DemuxThread> demux_thread;
    std::unique_ptr<DecodeThread> decode_thread;
    std::unique_ptr<SeekController> seek_controller;
    bool use_hardware_decode = true;
    bool recreated_for_paused_hevc_seek = false;

    int video_width = 0;
    int video_height = 0;
    float video_aspect = 16.0f / 9.0f;
};

DecodeDeviceMode default_decode_device_mode(AVCodecID codec_id);

class TrackPipelineManager {
public:
    using Storage = std::array<std::unique_ptr<TrackPipeline>, kMaxTracks>;
    using iterator = Storage::iterator;
    using const_iterator = Storage::const_iterator;
    using TrackCallback = std::function<void(size_t slot, TrackPipeline& track)>;
    using MoveCallback = std::function<void(size_t from, size_t to, TrackPipeline& track)>;

    std::unique_ptr<TrackPipeline>& operator[](size_t slot) { return tracks_[slot]; }
    const std::unique_ptr<TrackPipeline>& operator[](size_t slot) const { return tracks_[slot]; }

    iterator begin() { return tracks_.begin(); }
    iterator end() { return tracks_.end(); }
    const_iterator begin() const { return tracks_.begin(); }
    const_iterator end() const { return tracks_.end(); }

    int find_empty_slot() const;
    int find_slot_by_file_id(int file_id) const;
    void clear();
    void stop_all(const TrackCallback& before_stop = {});
    void stop_slot(size_t slot, const TrackCallback& before_stop = {});
    void compact_from(size_t slot, const MoveCallback& after_move = {});

    std::unique_ptr<TrackPipeline> create_pipeline(
        const std::string& path,
        bool hw_decode,
        const SeekRequest* initial_seek = nullptr) const;

private:
    Storage tracks_{};
};

} // namespace vr
