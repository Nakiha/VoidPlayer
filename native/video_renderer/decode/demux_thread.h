#pragma once
#include "video_renderer/buffer/packet_queue.h"
#include <thread>
#include <atomic>
#include <string>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace vr {

struct DemuxStats {
    int video_stream_index = -1;
    int64_t duration_us = 0;
    int width = 0;
    int height = 0;
    AVRational time_base = {0, 1};
    AVCodecParameters* codec_params = nullptr; // borrowed, do not free
};

class DemuxThread {
public:
    DemuxThread(const std::string& file_path, PacketQueue& output_queue);
    ~DemuxThread();

    bool start();
    void stop();
    void seek(int64_t target_pts_us);

    const DemuxStats& stats() const { return stats_; }
    AVFormatContext* format_context() const { return fmt_ctx_; }

private:
    void run();
    int64_t pts_to_us(int64_t pts, AVRational time_base) const;

    std::string file_path_;
    PacketQueue& output_queue_;
    AVFormatContext* fmt_ctx_ = nullptr;
    DemuxStats stats_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> seeking_{false};
    std::atomic<int64_t> seek_target_us_{0};
};

} // namespace vr
