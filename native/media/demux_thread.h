#pragma once
#include "media/packet_queue.h"
#include "media/seek_controller.h"
#include <thread>
#include <atomic>
#include <string>
#include <cstdint>
#include <functional>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace vr {

enum class DemuxStreamKind {
    Video,
    Audio,
};

struct DemuxStats {
    int video_stream_index = -1;
    int audio_stream_index = -1;
    int64_t duration_us = 0;
    int width = 0;
    int height = 0;
    AVRational time_base = {0, 1};
    AVRational audio_time_base = {0, 1};
    int sar_num = 1;
    int sar_den = 1;
    int sample_rate = 0;
    int channels = 0;
    /// Borrowed pointer into AVStream->codecpar. Valid only while the
    /// DemuxThread's internal AVFormatContext is alive (i.e. after start()
    /// and before stop()). Do NOT free.
    AVCodecParameters* codec_params = nullptr;
    AVCodecParameters* audio_codec_params = nullptr;
};

class DemuxThread {
public:
    using SeekCallback = std::function<void(int64_t target_pts_us, SeekType type)>;

    DemuxThread(const std::string& file_path, SeekController& seek_controller);
    DemuxThread(const std::string& file_path, PacketQueue& output_queue,
                SeekController& seek_controller);
    ~DemuxThread();

    bool start();
    void stop();

    /// Register an output queue for packets of a media stream kind.
    /// Must be called before start(). The legacy constructor registers video.
    bool add_output(DemuxStreamKind kind, PacketQueue& output_queue);
    bool add_optional_output(DemuxStreamKind kind, PacketQueue& output_queue);

    void set_seek_callback(SeekCallback cb);

    const DemuxStats& stats() const { return stats_; }
    AVFormatContext* format_context() const { return fmt_ctx_; }

private:
    struct OutputRoute {
        DemuxStreamKind kind;
        int stream_index = -1;
        PacketQueue* queue = nullptr;
        bool optional = false;
    };

    void run();
    void abort_outputs();
    void flush_outputs();
    void signal_outputs_eof();
    int stream_index_for_kind(DemuxStreamKind kind) const;
    AVRational time_base_for_stream(int stream_index) const;

    std::string file_path_;
    SeekController& seek_controller_;
    AVFormatContext* fmt_ctx_ = nullptr;
    DemuxStats stats_;
    std::vector<OutputRoute> output_routes_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    SeekCallback seek_callback_;
};

} // namespace vr
