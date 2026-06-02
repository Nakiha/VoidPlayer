#pragma once

#include "renderer/decode/exact_seek_window.h"

#include <cstddef>
#include <cstdint>

namespace vr {

class DecodedFramePublisher;
class ExactSeekCandidateStore;
class HwDecodeProvider;
class TrackBuffer;

struct ExactSeekPreviewFramePublishResult {
    bool can_publish = false;
    bool conversion_failed = false;
    int64_t selected_pts_us = 0;
    size_t published_count = 0;
    size_t pending_count = 0;
};

ExactSeekPreviewFramePublishResult publish_exact_seek_preview_frames(
    ExactSeekCandidateStore& candidates,
    size_t selected,
    TrackBuffer& output_buffer,
    DecodedFramePublisher& publisher,
    bool hw_enabled,
    HwDecodeProvider* hw_provider,
    bool& hw_visibility_flush_pending,
    size_t max_window_frames = kExactSeekPreviewWindowFrames);

bool publish_pending_exact_seek_frame(ExactSeekCandidateStore& candidates,
                                      DecodedFramePublisher& publisher);

} // namespace vr
