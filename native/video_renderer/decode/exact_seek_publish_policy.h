#pragma once

#include "video_renderer/buffer/track_buffer.h"

#include <cstddef>
#include <cstdint>

namespace vr {

struct ExactSeekPreviewPublishWindow {
    bool can_publish = false;
    size_t end = 0;
    size_t published = 0;
};

struct ExactSeekPreviewPublishCompletion {
    bool apply = false;
    TrackState output_state = TrackState::Ready;
    bool pause_decode = false;
    bool post_seek = false;
    int64_t exact_seek_target_us = -1;
    bool drain_decoder_before_next_packet = true;
    int64_t selected_pts_us = 0;
    size_t published_count = 0;
    size_t pending_count = 0;
};

ExactSeekPreviewPublishWindow choose_exact_seek_preview_publish_window(
    size_t selected,
    size_t reorder_count,
    size_t buffered_frames,
    size_t buffer_capacity,
    size_t max_window_frames);

ExactSeekPreviewPublishCompletion complete_exact_seek_preview_publish(
    bool pause_after_preroll);

ExactSeekPreviewPublishCompletion plan_exact_seek_preview_completion(
    bool can_publish,
    bool conversion_failed,
    bool pause_after_preroll,
    int64_t selected_pts_us,
    size_t published_count,
    size_t pending_count);

} // namespace vr
