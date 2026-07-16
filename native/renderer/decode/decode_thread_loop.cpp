#include "renderer/decode/decode_thread.h"

#include "renderer/decode/decode_perf_timing.h"

#include "renderer/decode/av_frame_lifetime.h"
#include "renderer/decode/codec_loop.h"
#include "renderer/decode/decode_drain_policy.h"
#include "renderer/decode/decode_exact_seek_reorder.h"
#include "renderer/decode/decode_frame_drainer.h"
#include "renderer/decode/decode_frame_receive_loop.h"
#include "renderer/decode/decode_loop_policy.h"
#include "renderer/decode/decode_packet_sender.h"
#include "renderer/decode/frame_timestamp_rescaler.h"

#include <spdlog/spdlog.h>

#include <chrono>

namespace vr {

bool DecodeThread::handle_queue_gap_or_eof(
    AVFrame* frame,
    const std::function<void(AVFrame*)>& rescale_ts,
    DecodedFramePublisher& publisher) {
    // EOF flush: drain codec once when the producer signals EOF.
    // Skip during Buffering (post-seek preroll) — the DemuxThread may signal
    // EOF very quickly after seek (file is cached), but we need to keep
    // decoding to fill the preroll buffer first.
    const auto eof_action = choose_eof_drain_action(
        input_queue_.is_eof(),
        eof_flushed_,
        output_buffer_.state(),
        exact_seek_target_us_ >= 0);
    if (eof_action == EofDrainAction::None) {
        return false;
    }

    if (eof_action == EofDrainAction::BufferingExactSeekDrain ||
        eof_action == EofDrainAction::BufferingMarkFlushed) {
        return handle_buffering_eof(eof_action, frame, rescale_ts);
    }

    return drain_codec_at_eof(frame, rescale_ts, publisher);
}

bool DecodeThread::handle_buffering_eof(
    EofDrainAction eof_action,
    AVFrame* frame,
    const std::function<void(AVFrame*)>& rescale_ts) {
    if (eof_action == EofDrainAction::BufferingExactSeekDrain) {
        drain_codec(frame, rescale_ts, exact_seek_target_us_);
        spdlog::info("[DecodeThread] Exact seek EOF drain: candidate window has {} frames",
                     exact_seek_candidates_.reorder_count());
        publish_best_exact_seek_frame();
    } else {
        eof_flushed_ = true;
        flush_reorder_buffer();
    }

    // Preroll check may complete if reorder flush added frames. Even with 0
    // frames, transition to Ready: the seek target is past this track duration.
    if (post_seek_) {
        spdlog::info("[DecodeThread] === Preroll complete (EOF): {} frames, state->Ready",
                     output_buffer_.total_count());
        output_buffer_.set_state(TrackState::Ready);
        post_seek_ = false;
    } else {
        spdlog::info("[DecodeThread] EOF seen during Buffering, deferring codec flush "
                     "(buf={}, pq={})",
                     output_buffer_.total_count(), input_queue_.size());
        if (should_complete_buffering_eof_preroll(output_buffer_.total_count())) {
            spdlog::info("[DecodeThread] === Preroll complete (EOF hold): {} frames, state->Ready",
                         output_buffer_.total_count());
            output_buffer_.set_state(TrackState::Ready);
            if (pause_after_preroll_.load(std::memory_order_acquire)) {
                decode_paused_.store(true, std::memory_order_release);
            }
        }
    }
    return false;
}

bool DecodeThread::drain_codec_at_eof(
    AVFrame* frame,
    const std::function<void(AVFrame*)>& rescale_ts,
    DecodedFramePublisher& publisher) {
    int send_ret = send_codec_packet_seh_guarded(codec_ctx_.get(), nullptr);
    const auto send_action = choose_eof_codec_send_action(send_ret);
    if (send_action != EofCodecSendAction::ReceiveFrames) {
        if (send_action == EofCodecSendAction::StopWithError) {
            stop_decode_loop_with_error();
        }
        return true;
    }
    while (true) {
        int ret = receive_codec_frame_seh_guarded(codec_ctx_.get(), frame);
        const auto receive_action = choose_eof_codec_receive_action(ret);
        if (receive_action == DecodeDrainReceiveAction::StopWithError) {
            stop_decode_loop_with_error();
            break;
        }
        if (receive_action == DecodeDrainReceiveAction::Stop) {
            break;
        }

        AvFrameUnrefGuard frame_guard(frame);
        rescale_ts(frame);
        log_hw_frame_context_once(frame);
        publisher.flush_before_publish_if_needed();
        if (!publisher.convert_and_push_frame(frame, "EOF drain")) {
            break;
        }
    }
    // Flush decode device after EOF drain to ensure shared NV12 textures are
    // visible to the render device.
    if (hw_enabled_ && hw_provider_) {
        hw_provider_->flush();
    }
    eof_flushed_ = true;
    return false;
}

DecodedFramePublisher DecodeThread::make_frame_publisher() {
    return DecodedFramePublisher(output_buffer_,
                                 converter_,
                                 hw_enabled_,
                                 hw_provider_,
                                 hw_visibility_flush_pending_,
                                 decode_paused_,
                                 running_,
                                 &stage_perf_);
}

struct DecodeThread::DecodeLoopScratch {
    AVFrame* frame = nullptr;
    DecodedFramePublisher& publisher;
    std::function<void(AVFrame*)> rescale_timestamps;
};

void DecodeThread::run() {
    spdlog::info("[DecodeThread] Decode loop started (hw={})", hw_enabled_);

    auto frame_owner = AvFrameOwner::allocate();
    if (!frame_owner) {
        spdlog::error("[DecodeThread] Failed to allocate frame");
        output_buffer_.set_state(TrackState::Error);
        return;
    }
    AVFrame* frame = frame_owner.get();
    auto rescale_ts = [&](AVFrame* frame_to_rescale) {
        const auto result = timestamp_normalizer_.normalize(frame_to_rescale);
        if (result.adjusted_for_monotonicity &&
            (result.adjustment_count <= 8 ||
             result.adjustment_count % 120 == 0)) {
            spdlog::warn(
                "[DecodeTimestamp] normalized non-monotonic timestamp "
                "raw_pts_us={} best_effort_pts_us={} output_pts_us={} "
                "used_best_effort={} correction={} order_preserved=true dropped=0",
                result.raw_pts_available ? result.raw_pts_us : AV_NOPTS_VALUE,
                result.best_effort_available
                    ? result.best_effort_pts_us
                    : AV_NOPTS_VALUE,
                result.output_pts_us,
                result.used_best_effort,
                result.adjustment_count);
        }
    };
    auto publisher = make_frame_publisher();
    DecodeLoopScratch scratch{
        frame,
        publisher,
        rescale_ts,
    };

    while (running_.load()) {
        if (run_decode_loop_step(scratch) == DecodeLoopStepResult::Stop) {
            break;
        }
    }

    output_buffer_.set_state(TrackState::Flushing);
    spdlog::info("[DecodeThread] Decode loop ended");
}

DecodeThread::DecodeLoopStepResult DecodeThread::run_decode_loop_step(
    DecodeLoopScratch& scratch) {
    AVFrame* frame = scratch.frame;
    auto& publisher = scratch.publisher;
    auto& rescale_ts = scratch.rescale_timestamps;

    const auto seek_notification = take_pending_seek_notification();
    if (seek_notification.has_value()) {
        begin_seek_epoch(frame, *seek_notification);
        return DecodeLoopStepResult::Continue;
    }

    if (should_publish_pending_exact_seek_frames(
            exact_seek_candidates_.pending_count(),
            decode_paused_.load(std::memory_order_acquire),
            output_buffer_.state())) {
        publish_pending_exact_seek_frames();
        return DecodeLoopStepResult::Continue;
    }

    if (should_drain_decoder_before_next_packet(
            drain_decoder_before_next_packet_,
            decode_paused_.load(std::memory_order_acquire),
            output_buffer_.state())) {
        return drain_before_next_packet(scratch);
    }

    // Fully pause decode consumption so the packet queue preserves packets
    // and the demux thread stops at backpressure instead of racing to EOF.
    if (should_pause_decode_consumption(
            decode_paused_.load(std::memory_order_acquire),
            output_buffer_.state())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return DecodeLoopStepResult::Continue;
    }

    // Non-blocking pop with short sleep allows seek_pending to be checked promptly.
    PacketPopResult packet_result = input_queue_.try_pop();
    auto packet = std::move(packet_result.packet);
    const auto pop_action = choose_decode_packet_pop_action(
        packet_result.status,
        packet.get() != nullptr,
        running_.load(std::memory_order_acquire),
        cancelled_.load(std::memory_order_acquire));
    if (pop_action != DecodePacketPopAction::ProcessPacket) {
        if (pop_action == DecodePacketPopAction::SleepAndContinue) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return DecodeLoopStepResult::Continue;
        }
        if (handle_queue_gap_or_eof(frame, rescale_ts, publisher)) {
            return DecodeLoopStepResult::Stop;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return DecodeLoopStepResult::Continue;
    }

    eof_flushed_ = false;

    return process_decode_packet(packet, scratch);
}

DecodeThread::DecodeLoopStepResult DecodeThread::drain_before_next_packet(
    DecodeLoopScratch& scratch) {
    AVFrame* frame = scratch.frame;
    auto& publisher = scratch.publisher;
    auto& rescale_ts = scratch.rescale_timestamps;

    const auto drain_result = drain_frames_before_next_packet(
        frame,
        DecodeFrameDrainCallbacks{
            [this]() {
                return should_abort_drain_before_receive(
                    cancelled_.load(std::memory_order_acquire),
                    output_buffer_.state());
            },
            [this](AVFrame* frame_to_receive) {
                return receive_codec_frame_seh_guarded(
                    codec_ctx_.get(), frame_to_receive, hw_enabled_, device_mutex_);
            },
            rescale_ts,
            [this](const AVFrame* ready_frame) {
                log_hw_frame_context_once(ready_frame);
            },
            [&publisher](AVFrame* frame_to_publish) {
                publisher.flush_before_publish_if_needed(true);
                return publisher.convert_and_push_frame(
                    frame_to_publish, "drain before next packet");
            },
            [this]() {
                return should_stop_drain_after_publish(
                    decode_paused_.load(std::memory_order_acquire),
                    output_buffer_.state());
            },
        });
    if (drain_result.stop_with_error) {
        return stop_decode_loop_with_error();
    }
    if (drain_result.clear_drain_request) {
        drain_decoder_before_next_packet_ = false;
    }
    if (drain_result.frames_published > 0) {
        perf_.frames_decoded.fetch_add(
            static_cast<uint64_t>(drain_result.frames_published),
            std::memory_order_relaxed);
    }
    return DecodeLoopStepResult::Continue;
}

DecodeThread::DecodeLoopStepResult DecodeThread::process_decode_packet(
    AvPacketOwner& packet,
    DecodeLoopScratch& scratch) {
    AVFrame* frame = scratch.frame;
    auto& publisher = scratch.publisher;
    auto& rescale_ts = scratch.rescale_timestamps;

    auto batch_t0 = std::chrono::steady_clock::now();
    const uint64_t publish_wait_before_us =
        stage_perf_.publish_wait_total_us.load(std::memory_order_relaxed);
    const auto packet_send_t0 = std::chrono::steady_clock::now();
    const auto packet_send_result = send_decode_packet(
        packet,
        DecodePacketSendCallbacks{
            [this]() {
                // If decode is paused (seek transition), discard the
                // packet without sending to codec. This prevents the HEVC
                // decoder from emitting stale-reference warnings.
                return should_discard_packet_before_decode(
                    has_pending_seek_notification(),
                    decode_paused_.load(std::memory_order_acquire),
                    output_buffer_.state());
            },
            [this]() {
                // Cancel checkpoint: abort if a new seek arrived while
                // this loop was waiting on queue/publisher backpressure.
                return should_abort_packet_before_send(
                    cancelled_.load(std::memory_order_acquire));
            },
            [this](AVPacket* packet_to_send) {
                return send_codec_packet_seh_guarded(
                    codec_ctx_.get(), packet_to_send, hw_enabled_, device_mutex_);
            },
            [](int send_ret) {
                spdlog::error("[DecodeThread] Error sending packet: {:#x}",
                              static_cast<unsigned>(send_ret));
            },
        });
    stage_perf_.record_packet_send(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - packet_send_t0).count()));
    if (packet_send_result.stop_with_error) {
        return stop_decode_loop_with_error();
    }
    if (!packet_send_result.sent) {
        return DecodeLoopStepResult::Continue;
    }

    const auto receive_loop_t0 = std::chrono::steady_clock::now();
    const auto receive_result = receive_decode_frames_for_packet(
        frame,
        DecodeFrameReceiveLoopOptions{
            exact_seek_target_us_ >= 0,
            exact_seek_target_us_,
            perf_.frames_decoded.load(std::memory_order_relaxed) > 0,
        },
        DecodeFrameReceiveLoopCallbacks{
            [this]() {
                return should_stop_receive_loop_before_frame(
                    cancelled_.load(std::memory_order_acquire));
            },
            [this](AVFrame* frame_to_receive) {
                return receive_codec_frame_seh_guarded(
                    codec_ctx_.get(), frame_to_receive, hw_enabled_, device_mutex_);
            },
            rescale_ts,
            [this](const AVFrame* ready_frame) {
                log_hw_frame_context_once(ready_frame);
            },
            [this]() {
                perf_.frames_dropped.fetch_add(1, std::memory_order_relaxed);
            },
            [this](AVFrame* exact_seek_frame) {
                auto candidate =
                    ExactSeekCandidateStore::make_candidate(exact_seek_frame);
                if (candidate.frame) {
                    collect_exact_seek_candidate(std::move(candidate));
                }
            },
            [this]() {
                return exact_seek_preview_window_ready();
            },
            [this]() {
                publish_best_exact_seek_frame();
            },
            [this]() {
                // Flush the independent decode device after the first
                // visible HW frame on startup and after seek/add-track
                // transitions. Without this, the render device can sample
                // a partially-written NV12 surface.
                hw_visibility_flush_pending_ = hw_enabled_;
            },
            [&publisher](AVFrame* frame_to_publish) {
                // The flush must happen before push_frame() publishes this
                // frame to the render thread, otherwise the paused preview
                // path can win the race and draw an incomplete surface.
                publisher.flush_visibility_if_needed();
                return publisher.convert_and_push_frame(
                    frame_to_publish, "decode loop");
            },
            [this]() {
                complete_preroll_if_ready();
            },
            [](int receive_ret) {
                spdlog::error("[DecodeThread] Error receiving frame: {:#x}",
                              static_cast<unsigned>(receive_ret));
            },
        });
    const auto receive_loop_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - receive_loop_t0).count());
    if (receive_result.stop_with_error) {
        return stop_decode_loop_with_error();
    }
    const int frames_produced = receive_result.frames_produced;
    if (frames_produced > 0) {
        stage_perf_.record_receive_loop(
            receive_loop_us,
            static_cast<uint64_t>(frames_produced));
    }

    // Exact seek B-frame reordering fallback. The receive loop normally
    // publishes once enough frames are collected, but EOF/drain can also
    // make the buffer ready here.
    if (!exact_seek_candidates_.reorder_empty()) {
        const DecodeExactSeekReorderState reorder_state{
            exact_seek_target_us_ >= 0,
            exact_seek_candidates_.reorder_count(),
            input_queue_.is_eof(),
            input_queue_.size(),
            eof_flushed_,
            exact_seek_preview_window_ready(),
        };
        handle_exact_seek_reorder_after_receive(
            reorder_state,
            DecodeExactSeekReorderCallbacks{
                [this, frame, &rescale_ts]() {
                    drain_codec(frame, rescale_ts, exact_seek_target_us_);
                },
                [this]() {
                    return exact_seek_candidates_.reorder_count();
                },
                [](size_t reorder_count) {
                    spdlog::info("[DecodeThread] Exact seek EOF: codec drain, "
                                 "candidate window now has {} frames",
                                 reorder_count);
                },
                [this]() {
                    publish_best_exact_seek_frame();
                },
                [this]() -> std::optional<int64_t> {
                    auto first = output_buffer_.peek(0);
                    if (!first.has_value()) {
                        return std::nullopt;
                    }
                    return first->pts_us;
                },
                [](std::optional<int64_t> first_pts_us) {
                    spdlog::info("[DecodeThread] Exact seek candidate window: frames pushed, "
                                 "first_pts={:.3f}s",
                                 first_pts_us.has_value()
                                     ? static_cast<double>(*first_pts_us) / 1e6
                                     : -1.0);
                },
            });
    }

    complete_preroll_if_ready();

    // Hardware HEVC exact seek is sensitive to burst-feeding packets while
    // paused. Playback naturally paces this path through render/clock
    // consumption; mirror a tiny amount of that pacing during drain mode.
    if (should_pace_hardware_exact_seek_decode(
            exact_seek_target_us_ >= 0, hw_enabled_)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (frames_produced > 0) {
        const uint64_t elapsed_batch_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - batch_t0).count());
        const uint64_t batch_us = decode_active_batch_time_us(
            elapsed_batch_us,
            publish_wait_before_us,
            stage_perf_.publish_wait_total_us.load(std::memory_order_relaxed));
        perf_.frames_decoded.fetch_add(frames_produced, std::memory_order_relaxed);
        perf_.total_decode_us.fetch_add(batch_us, std::memory_order_relaxed);
        // Update peak (CAS loop)
        uint64_t cur_max = perf_.max_decode_us.load(std::memory_order_relaxed);
        while (batch_us > cur_max &&
               !perf_.max_decode_us.compare_exchange_weak(cur_max, batch_us,
                                                          std::memory_order_relaxed)) {}
        spdlog::debug(
            "[DecodeThread] Decoded {} frames active={}us elapsed={}us, "
            "buf_state={}, buf_count={}",
            frames_produced, batch_us, elapsed_batch_us,
            static_cast<int>(output_buffer_.state()),
            output_buffer_.total_count());
    }

    return DecodeLoopStepResult::Continue;
}

DecodeThread::DecodeLoopStepResult DecodeThread::stop_decode_loop_with_error() {
    output_buffer_.set_state(TrackState::Error);
    decode_paused_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);
    return DecodeLoopStepResult::Stop;
}

} // namespace vr
