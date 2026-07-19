#pragma once
#include "media/media_input_session.h"
#include "media/packet_queue.h"
#include "media/seek_controller.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <mutex>
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

class DemuxThread {
public:
    using SeekCallback = std::function<void(int64_t target_pts_us, SeekType type)>;
    using ErrorCallback = std::function<void(int error_code)>;

    DemuxThread(const std::string& file_path, SeekController& seek_controller);
    DemuxThread(const std::string& file_path, PacketQueue& output_queue,
                SeekController& seek_controller);
    ~DemuxThread();

    /// Open/probe the input and populate stats without starting packet reads.
    /// Output routes must already be registered.
    bool open();

    /// Start the packet-reading worker after open() and callback wiring.
    bool start_thread();

    /// Backward-compatible one-shot open + start_thread.
    bool start();
    void stop();

    /// Register an output queue for packets of a media stream kind.
    /// Must be called before start(). The legacy constructor registers video.
    bool add_output(DemuxStreamKind kind, PacketQueue& output_queue);
    bool add_optional_output(DemuxStreamKind kind, PacketQueue& output_queue);

    void set_seek_callback(SeekCallback cb);
    void set_error_callback(ErrorCallback cb);
    void fail_next_read_for_test(int error_code);

    const DemuxStats& stats() const { return stats_; }
    AVFormatContext* format_context() const {
        return input_.format_context();
    }

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
    void emit_error(int error_code);
    int stream_index_for_kind(DemuxStreamKind kind) const;
    AVRational time_base_for_stream(int stream_index) const;

    std::string file_path_;
    SeekController& seek_controller_;
    MediaInputSession input_;
    DemuxStats stats_;
    std::vector<OutputRoute> output_routes_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    SeekCallback seek_callback_;
    mutable std::mutex seek_callback_mutex_;
    ErrorCallback error_callback_;
    mutable std::mutex error_callback_mutex_;
    std::atomic<int> forced_read_error_for_test_{0};
    int32_t next_video_packet_identity_index_ = 0;
    std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    bool opening_ = false;
};

} // namespace vr
