#pragma once
#include "video_renderer/buffer/packet_queue.h"
#include "video_renderer/buffer/track_buffer.h"
#include "video_renderer/decode/frame_converter.h"
#include "video_renderer/decode/hw/hw_decode_provider.h"
#include "video_renderer/sync/seek_controller.h"
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <functional>
#include <deque>
#include <optional>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace vr {

/// Performance stats snapshot for a single decode thread.
struct DecodePerfCounters {
    std::atomic<uint64_t> frames_decoded{0};       ///< Total frames decoded since start
    std::atomic<uint64_t> total_decode_us{0};       ///< Cumulative decode time (microseconds)
    std::atomic<uint64_t> max_decode_us{0};         ///< Peak decode time for a single batch (microseconds)
    std::atomic<uint64_t> frames_dropped{0};        ///< Frames discarded during exact seek

    /// Snapshot current values (thread-safe).
    struct Snapshot {
        uint64_t frames_decoded;
        uint64_t total_decode_us;
        uint64_t max_decode_us;
        uint64_t frames_dropped;
    };
    Snapshot snapshot() const {
        return {
            frames_decoded.load(std::memory_order_relaxed),
            total_decode_us.load(std::memory_order_relaxed),
            max_decode_us.load(std::memory_order_relaxed),
            frames_dropped.load(std::memory_order_relaxed),
        };
    }
};

class DecodeThread {
public:
    DecodeThread(PacketQueue& input_queue, TrackBuffer& output_buffer,
                 const AVCodecParameters* codec_params, AVRational time_base);
    ~DecodeThread();

    /// Returns true if the decoder was successfully initialized in the constructor.
    /// If false, start() will always fail — caller should not use this instance.
    bool is_valid() const { return codec_ctx_ != nullptr; }

    /// Enable hardware decode using the given native device.
    /// Must be called before start(). On failure, falls back to software.
    /// @param device_mutex  Shared mutex for D3D11 immediate context serialization.
    ///                      Must outlive this DecodeThread.
    bool enable_hardware_decode(void* native_device,
                                std::recursive_mutex* device_mutex = nullptr);

    bool start();
    void stop();

    /// Called from DemuxThread seek callback to notify this thread of a seek.
    void notify_seek(int64_t target_pts_us, SeekType type);

    /// Pause/resume packet processing. Set pause=true BEFORE requesting seek
    /// to prevent stale packets from being sent to the codec (avoids HEVC
    /// "Could not find ref" warnings during the seek transition).
    void set_decode_paused(bool paused);
    void set_pause_after_preroll(bool enabled);

    /// Read-only access to performance counters.
    const DecodePerfCounters& perf_counters() const { return perf_; }

    bool is_hardware_decode_enabled() const { return hw_enabled_; }
    AVCodecID codec_id() const { return codec_params_ ? codec_params_->codec_id : AV_CODEC_ID_NONE; }

private:
    struct ExactSeekCandidate {
        int64_t pts_us = 0;
        std::shared_ptr<AVFrame> frame;
        std::optional<TextureFrame> stable_frame;
    };

    void run();

    /// Attempt to open codec. Returns true on success.
    /// If hw_enabled_ is true and open fails, falls back to software.
    bool open_codec();

    /// Recreate codec_ctx_ for the requested decoder and copy stream params.
    bool reset_codec_context(const AVCodec* codec);

    /// Preferred software fallback decoder. For AV1 this is libdav1d when available.
    const AVCodec* preferred_software_decoder() const;

    /// Whether hardware frames are downloaded before being published.
    bool hardware_output_downloads_to_cpu() const;

    /// Whether decoded hardware surfaces can be held by the render queue.
    bool hardware_surfaces_are_renderer_owned() const;

    /// Drain remaining frames from the codec (avcodec_send_packet(nullptr) + receive loop).
    /// If target_us >= 0, frames with pts >= target_us are added to exact_seek_reorder_.
    /// Sets eof_flushed_ = true.
    void drain_codec(AVFrame* frame, const std::function<void(AVFrame*)>& rescale_ts, int64_t target_us = -1);

    /// Push currently collected exact-seek frames in decoder presentation order.
    void flush_reorder_buffer();

    /// Keep an exact-seek candidate alive without converting its pixels yet.
    ExactSeekCandidate make_exact_seek_candidate(AVFrame* frame) const;

    /// Add a candidate in decoder presentation order, retaining only the last pre-target frame.
    void collect_exact_seek_candidate(ExactSeekCandidate candidate);

    /// Snapshot a candidate that may become the paused exact-seek preview.
    void snapshot_exact_seek_candidate_if_needed(ExactSeekCandidate& candidate);

    /// Whether the collected stream-ordered candidates are enough to publish preview.
    bool exact_seek_preview_window_ready() const;

    /// Publish the selected exact-seek preview frame plus later decoded frames.
    void publish_exact_seek_window(size_t selected);

    /// Pick the closest collected frame before the exact seek target and publish it.
    bool publish_best_exact_seek_frame();

    /// Push decoded exact-seek frames that did not fit in the initial preview window.
    void publish_pending_exact_seek_frames();

    /// Log the FFmpeg hardware frame pool geometry once it is materialized.
    void log_hw_frame_context_once(const AVFrame* frame);

    /// Flush codec buffers after seek.
    void safe_flush_codec();

    /// Flush decode-device writes so the render device can safely sample the
    /// first hardware frame after startup/seek.
    void flush_hw_visibility_if_needed();

    /// Flush decode-device writes before publishing a hardware frame.
    void flush_hw_before_publish_if_needed(bool force_for_shared_surface = false);

    PacketQueue& input_queue_;
    TrackBuffer& output_buffer_;
    FrameConverter converter_;

    AVCodecContext* codec_ctx_ = nullptr;
    const AVCodec* codec_ = nullptr;
    const AVCodecParameters* codec_params_;
    AVRational time_base_;

    // Hardware decode state
    void* native_device_ = nullptr;
    AVBufferRef* hw_device_ctx_ = nullptr;   // Owned, from provider
    bool hw_enabled_ = false;
    HwDecodeType hw_type_ = HwDecodeType::None;
    std::unique_ptr<HwDecodeProvider> hw_provider_;  // Holds mutex lifetime
    AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;  // Per-instance, avoids global shared state
    std::recursive_mutex* device_mutex_ = nullptr;  // Shared D3D11 mutex for hw decode serialization
    bool hw_frames_ctx_logged_ = false;

    // Seek coordination — protected by seek_mutex_ to avoid torn reads
    // between seek_target / seek_type / seek_pending
    std::mutex seek_mutex_;
    struct SeekState {
        bool pending = false;
        int64_t target_pts_us = 0;
        SeekType type = SeekType::Keyframe;
    };
    SeekState seek_;

    std::atomic<bool> cancelled_{false};     // Set by notify_seek() to abort in-progress decode
    std::atomic<bool> decode_paused_{false};
    std::atomic<bool> pause_after_preroll_{false};
    int64_t exact_seek_target_us_ = -1;  // >= 0 when discarding frames before exact seek target
    std::vector<ExactSeekCandidate> exact_seek_reorder_;  // Stream-ordered exact-seek candidates
    std::deque<ExactSeekCandidate> exact_seek_pending_frames_;  // Post-preview frames for smooth play
    bool drain_decoder_before_next_packet_ = false;

    bool eof_flushed_ = false;
    bool post_seek_ = false;      // After seek: transition to Ready after 1 frame instead of full preroll
    bool hw_visibility_flush_pending_ = false;

    std::thread thread_;
    std::atomic<bool> running_{false};
    DecodePerfCounters perf_;
};

} // namespace vr
