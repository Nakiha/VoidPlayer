#pragma once
#include "media/ffmpeg_lifetime.h"
#include "media/packet_queue.h"
#include "renderer/decode/codec_loop.h"
#include "renderer/buffer/track_buffer.h"
#include "renderer/decode/decoded_frame_publisher.h"
#include "renderer/decode/decode_seek_epoch.h"
#include "renderer/decode/exact_seek_candidate_store.h"
#include "renderer/decode/frame_converter.h"
#include "renderer/decode/hw/hw_decode_provider.h"
#include "media/seek_controller.h"
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>
#include <functional>
#include <deque>
#include <optional>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace vr {

enum class EofDrainAction;

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

struct DecodeMemoryStats {
    bool hardware_enabled = false;
    bool hardware_download_to_cpu = false;
    int hw_format = AV_PIX_FMT_NONE;
    int sw_format = AV_PIX_FMT_NONE;
    int hw_width = 0;
    int hw_height = 0;
    int hw_initial_pool_size = 0;
    int extra_hw_frames = 0;
    uint64_t estimated_hw_frame_bytes = 0;
    uint64_t estimated_hw_pool_bytes = 0;
    size_t exact_seek_reorder_count = 0;
    size_t exact_seek_pending_count = 0;
    size_t exact_seek_stable_frame_count = 0;
    size_t exact_seek_budget_drop_count = 0;
    uint64_t exact_seek_candidate_cpu_bytes = 0;
    uint64_t exact_seek_stable_cpu_bytes = 0;
    D3D11SnapshotPoolStats snapshot_pool;
};

class DecodeThread {
public:
    DecodeThread(PacketQueue& input_queue, TrackBuffer& output_buffer,
                 const AVCodecParameters* codec_params, AVRational time_base);
    ~DecodeThread();

    /// Returns true if the decoder was successfully initialized in the constructor.
    /// If false, start() will always fail — caller should not use this instance.
    bool is_valid() const { return static_cast<bool>(codec_ctx_); }

    /// Enable hardware decode using an explicit decode device strategy.
    /// Must be called before start(). On failure, falls back to software.
    /// @param mode  Decode device ownership and sharing policy.
    /// @param render_device  Required only for SharedRenderDevice.
    /// @param device_mutex  Shared mutex for D3D11 immediate context serialization.
    ///                      Must outlive this DecodeThread.
    bool enable_hardware_decode(DecodeDeviceMode mode = DecodeDeviceMode::IndependentDevice,
                                void* render_device = nullptr,
                                std::recursive_mutex* device_mutex = nullptr,
                                RenderBackendKind backend = RenderBackendKind::D3D11);

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
    DecodeMemoryStats memory_stats() const;

    bool is_hardware_decode_enabled() const { return hw_enabled_; }
    AVCodecID codec_id() const { return codec_params_ ? codec_params_->codec_id : AV_CODEC_ID_NONE; }
    std::string decoder_name() const;
    void set_codec_open_for_test(CodecOpenFunction open_fn) { codec_open_for_test_ = open_fn; }

private:
    struct DecodeLoopScratch;
    enum class DecodeLoopStepResult {
        Continue,
        Stop,
    };

    void run();
    DecodeLoopStepResult run_decode_loop_step(DecodeLoopScratch& scratch);
    DecodeLoopStepResult drain_before_next_packet(DecodeLoopScratch& scratch);
    DecodeLoopStepResult process_decode_packet(AvPacketOwner& packet, DecodeLoopScratch& scratch);
    DecodeLoopStepResult stop_decode_loop_with_error();

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
    /// If target_us >= 0, frames with pts >= target_us are added to exact seek candidates.
    /// Sets eof_flushed_ = true.
    void drain_codec(AVFrame* frame, const std::function<void(AVFrame*)>& rescale_ts, int64_t target_us = -1);

    /// Push currently collected exact-seek frames in decoder presentation order.
    void flush_reorder_buffer();

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

    /// Complete the Buffering -> Ready preroll transition when the current
    /// output buffer has enough frames.
    bool complete_preroll_if_ready();

    /// Handle queue gaps and EOF codec draining. Returns true when the decode
    /// loop should stop immediately.
    bool handle_queue_gap_or_eof(AVFrame* frame,
                                 const std::function<void(AVFrame*)>& rescale_ts,
                                 DecodedFramePublisher& publisher);
    bool handle_buffering_eof(EofDrainAction eof_action,
                              AVFrame* frame,
                              const std::function<void(AVFrame*)>& rescale_ts);
    bool drain_codec_at_eof(AVFrame* frame,
                            const std::function<void(AVFrame*)>& rescale_ts,
                            DecodedFramePublisher& publisher);

    /// Create a lightweight publisher view over decode-thread-owned frame state.
    DecodedFramePublisher make_frame_publisher();

    /// Log the FFmpeg hardware frame pool geometry once it is materialized.
    void log_hw_frame_context_once(const AVFrame* frame);

    /// Atomically take the next pending seek notification, if any.
    std::optional<DecodeSeekNotification> take_pending_seek_notification();

    /// Check whether a seek notification is pending without consuming it.
    bool has_pending_seek_notification();

    /// Reset decode-thread state for a new seek epoch.
    void begin_seek_epoch(AVFrame* frame, const DecodeSeekNotification& notification);

    /// Flush codec buffers after seek.
    void safe_flush_codec();

    PacketQueue& input_queue_;
    TrackBuffer& output_buffer_;
    FrameConverter converter_;

    AvCodecContextOwner codec_ctx_;
    const AVCodec* codec_ = nullptr;
    const AVCodecParameters* codec_params_;
    AVRational time_base_;
    CodecOpenFunction codec_open_for_test_ = nullptr;

    // Hardware decode state
    void* native_device_ = nullptr;
    DecodeDeviceMode decode_device_mode_ = DecodeDeviceMode::IndependentDevice;
    AvBufferRefOwner hw_device_ctx_;   // Owned, from provider
    bool hw_enabled_ = false;
    HwDecodeType hw_type_ = HwDecodeType::None;
    std::unique_ptr<HwDecodeProvider> hw_provider_;  // Holds mutex lifetime
    AVPixelFormat hw_pix_fmt_ = AV_PIX_FMT_NONE;  // Per-instance, avoids global shared state
    std::recursive_mutex* device_mutex_ = nullptr;  // Shared D3D11 mutex for hw decode serialization
    bool hw_frames_ctx_logged_ = false;
    std::atomic<int> hw_frames_format_{AV_PIX_FMT_NONE};
    std::atomic<int> hw_frames_sw_format_{AV_PIX_FMT_NONE};
    std::atomic<int> hw_frames_width_{0};
    std::atomic<int> hw_frames_height_{0};
    std::atomic<int> hw_frames_initial_pool_size_{0};

    // Seek coordination — protected by seek_mutex_ to avoid torn reads
    // between seek_target / seek_type / seek_pending
    std::mutex seek_mutex_;
    DecodePendingSeekState seek_;

    std::atomic<bool> cancelled_{false};     // Set by notify_seek() to abort in-progress decode
    std::atomic<bool> decode_paused_{false};
    std::atomic<bool> pause_after_preroll_{false};
    int64_t exact_seek_target_us_ = -1;  // >= 0 when discarding frames before exact seek target
    bool exact_seek_prefer_after_target_ = false;
    ExactSeekCandidateStore exact_seek_candidates_;
    bool drain_decoder_before_next_packet_ = false;

    bool eof_flushed_ = false;
    bool post_seek_ = false;      // After seek: transition to Ready after 1 frame instead of full preroll
    bool hw_visibility_flush_pending_ = false;

    std::thread thread_;
    std::atomic<bool> running_{false};
    DecodePerfCounters perf_;
};

} // namespace vr
