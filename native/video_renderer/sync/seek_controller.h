#pragma once
#include <cstdint>
#include <functional>
#include <atomic>
#include <mutex>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

namespace vr {

enum class SeekType {
    Keyframe,
    Exact
};

struct SeekRequest {
    int64_t target_pts_us;
    SeekType type;
};

class SeekController {
public:
    SeekController();

    void request_seek(int64_t target_pts_us, SeekType type);
    bool has_pending_seek() const;
    SeekRequest pending_request() const;
    void clear_pending();

    bool execute_seek(AVFormatContext* fmt_ctx, int stream_index,
                      const AVRational& time_base,
                      AVCodecContext* codec_ctx = nullptr);

private:
    bool seek_keyframe(AVFormatContext* fmt_ctx, int stream_index,
                       const AVRational& time_base, int64_t target_pts_us);
    bool seek_exact(AVFormatContext* fmt_ctx, int stream_index,
                    const AVRational& time_base, AVCodecContext* codec_ctx,
                    int64_t target_pts_us);

    SeekRequest pending_;
    std::atomic<bool> has_pending_{false};
    mutable std::mutex pending_mutex_;
};

} // namespace vr
