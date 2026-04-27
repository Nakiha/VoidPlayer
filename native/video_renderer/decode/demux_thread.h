#pragma once
#include "video_renderer/buffer/packet_queue.h"
#include "video_renderer/sync/seek_controller.h"
#include <thread>
#include <atomic>
#include <string>
#include <cstdint>
#include <functional>

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
    int sar_num = 1;
    int sar_den = 1;
    /// Borrowed pointer into AVStream->codecpar. Valid only while the
    /// DemuxThread's internal AVFormatContext is alive (i.e. after start()
    /// and before stop()). Do NOT free.
    AVCodecParameters* codec_params = nullptr;
};

class DemuxThread {
public:
    using SeekCallback = std::function<void(int64_t target_pts_us, SeekType type)>;

    DemuxThread(const std::string& file_path, PacketQueue& output_queue,
                SeekController& seek_controller);
    ~DemuxThread();

    bool start();
    void stop();

    void set_seek_callback(SeekCallback cb);

    const DemuxStats& stats() const { return stats_; }
    AVFormatContext* format_context() const { return fmt_ctx_; }

private:
    void run();

    std::string file_path_;
    PacketQueue& output_queue_;
    SeekController& seek_controller_;
    AVFormatContext* fmt_ctx_ = nullptr;
    DemuxStats stats_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    SeekCallback seek_callback_;
};

} // namespace vr
