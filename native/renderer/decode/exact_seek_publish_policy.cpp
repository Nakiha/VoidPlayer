#include "renderer/decode/exact_seek_publish_policy.h"

#include <algorithm>

namespace vr {

ExactSeekPreviewPublishWindow choose_exact_seek_preview_publish_window(
    size_t selected,
    size_t reorder_count,
    size_t buffered_frames,
    size_t buffer_capacity,
    size_t max_window_frames) {
    ExactSeekPreviewPublishWindow window;
    if (selected >= reorder_count || buffer_capacity <= buffered_frames || max_window_frames == 0) {
        return window;
    }

    const size_t free_slots = buffer_capacity - buffered_frames;
    window.end = std::min(reorder_count, selected + std::min(max_window_frames, free_slots));
    window.published = window.end - selected;
    window.can_publish = window.published > 0;
    return window;
}

ExactSeekPreviewPublishCompletion complete_exact_seek_preview_publish(
    bool pause_after_preroll) {
    ExactSeekPreviewPublishCompletion completion;
    completion.apply = true;
    completion.pause_decode = pause_after_preroll;
    return completion;
}

ExactSeekPreviewPublishCompletion plan_exact_seek_preview_completion(
    bool can_publish,
    bool conversion_failed,
    bool pause_after_preroll,
    int64_t selected_pts_us,
    size_t published_count,
    size_t pending_count) {
    if (!can_publish || conversion_failed) {
        return {};
    }

    auto completion = complete_exact_seek_preview_publish(pause_after_preroll);
    completion.selected_pts_us = selected_pts_us;
    completion.published_count = published_count;
    completion.pending_count = pending_count;
    return completion;
}

} // namespace vr
