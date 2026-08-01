#include "renderer/decode/exact_seek_frame_publisher.h"

#include "renderer/buffer/track_buffer.h"
#include "renderer/decode/decoded_frame_publisher.h"
#include "renderer/decode/exact_seek_candidate_store.h"
#include "renderer/decode/exact_seek_publish_policy.h"
#include <spdlog/spdlog.h>

#include <optional>
#include <utility>

namespace vr {

ExactSeekPreviewFramePublishResult publish_exact_seek_preview_frames(
    ExactSeekCandidateStore& candidates,
    size_t selected,
    TrackBuffer& output_buffer,
    DecodedFramePublisher& publisher,
    size_t max_window_frames) {
    ExactSeekPreviewFramePublishResult result;
    if (selected >= candidates.reorder_count()) {
        return result;
    }

    publisher.flush_visibility_if_needed();
    result.selected_pts_us = candidates.reorder_at(selected).pts_us;
    const auto window = choose_exact_seek_preview_publish_window(
        selected,
        candidates.reorder_count(),
        output_buffer.total_count(),
        output_buffer.max_count(),
        max_window_frames);
    if (!window.can_publish) {
        spdlog::warn("[DecodeThread] Exact seek preview skipped: output buffer is full");
        return result;
    }

    result.can_publish = true;
    for (size_t i = window.start; i < window.end; ++i) {
        auto& candidate = candidates.reorder_at(i);
        if (!candidate.frame) {
            continue;
        }
        publisher.flush_before_publish_if_needed(true);

        std::optional<TextureFrame> frame;
        if (i == selected &&
            candidate.stable_frame.has_value() &&
            candidate.stable_frame->texture_handle) {
            frame = *candidate.stable_frame;
        } else {
            frame = publisher.convert_frame_for_publish(candidate.frame.get());
        }
        if (!publisher.push_converted_frame(std::move(frame), "publishing exact-seek window")) {
            candidates.clear();
            result.conversion_failed = true;
            return result;
        }
        if (i < selected) {
            ++result.history_count;
        }
    }
    for (size_t i = 0; i < result.history_count; ++i) {
        if (!output_buffer.advance_history_cursor()) {
            spdlog::error(
                "[DecodeThread] Failed to position exact-seek predecessor history");
            candidates.clear();
            result.conversion_failed = true;
            return result;
        }
    }

    const auto predecessor = output_buffer.peek(-1);
    const auto current = output_buffer.peek(0);
    spdlog::debug(
        "[DecodeThread] Exact seek publish cursor: history={} predecessor_pts={:.6f}s current_pts={:.6f}s",
        result.history_count,
        predecessor.has_value() ? predecessor->pts_us / 1e6 : -1.0,
        current.has_value() ? current->pts_us / 1e6 : -1.0);

    candidates.move_reorder_tail_to_pending(window.end);
    result.published_count = window.published;
    result.pending_count = candidates.pending_count();
    return result;
}

bool publish_pending_exact_seek_frame(ExactSeekCandidateStore& candidates,
                                      DecodedFramePublisher& publisher) {
    auto candidate = candidates.pop_pending();
    if (!candidate.has_value() || !candidate->frame) {
        return false;
    }
    publisher.flush_before_publish_if_needed(true);
    return publisher.convert_and_push_frame(candidate->frame.get(), "pending exact-seek frame");
}

} // namespace vr
