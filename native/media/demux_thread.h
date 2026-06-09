#pragma once
#include "media/ffmpeg_lifetime.h"
#include "media/packet_queue.h"
#include "media/seek_controller.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace vr {

class PrivateCdnFlvDemuxer;

namespace detail {
inline std::shared_ptr<AVCodecParameters> clone_codec_params(const AVCodecParameters* source) {
    if (!source) {
        return {};
    }

    AVCodecParameters* copy = avcodec_parameters_alloc();
    if (!copy) {
        return {};
    }
    if (avcodec_parameters_copy(copy, source) < 0) {
        avcodec_parameters_free(&copy);
        return {};
    }

    return std::shared_ptr<AVCodecParameters>(
        copy,
        [](AVCodecParameters* params) {
            avcodec_parameters_free(&params);
        });
}
} // namespace detail

enum class DemuxStreamKind {
    Video,
    Audio,
};

struct DemuxStats {
    int video_stream_index = -1;
    int audio_stream_index = -1;
    int64_t duration_us = 0;
    int64_t start_time_us = 0;
    int64_t bit_rate = 0;
    int width = 0;
    int height = 0;
    std::string format_name;
    std::string codec_name;
    std::string codec_long_name;
    AVRational time_base = {0, 1};
    AVRational audio_time_base = {0, 1};
    int sar_num = 1;
    int sar_den = 1;
    int sample_rate = 0;
    int channels = 0;
    /// Owned snapshots of the stream codec parameters. The raw pointers below
    /// are convenience aliases and remain valid while this DemuxStats copy lives.
    std::shared_ptr<AVCodecParameters> codec_params_owner;
    std::shared_ptr<AVCodecParameters> audio_codec_params_owner;
    AVCodecParameters* codec_params = nullptr;
    AVCodecParameters* audio_codec_params = nullptr;

    bool set_video_codec_params(const AVCodecParameters* source) {
        codec_params_owner = detail::clone_codec_params(source);
        codec_params = codec_params_owner.get();
        return codec_params != nullptr;
    }

    bool set_audio_codec_params(const AVCodecParameters* source) {
        audio_codec_params_owner = detail::clone_codec_params(source);
        audio_codec_params = audio_codec_params_owner.get();
        return audio_codec_params != nullptr;
    }
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
    AVFormatContext* format_context() const { return fmt_ctx_.get(); }

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
    static int interrupt_callback(void* opaque);

    std::string file_path_;
    SeekController& seek_controller_;
    AvFormatContextOwner fmt_ctx_;
    std::unique_ptr<PrivateCdnFlvDemuxer> private_flv_demuxer_;
    std::atomic<int64_t> open_deadline_ns_{0};
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
