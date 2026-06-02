#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace vr {

struct DecodeExactSeekReorderState {
    bool exact_seek_active = false;
    size_t reorder_count = 0;
    bool queue_eof = false;
    size_t queue_size = 0;
    bool eof_flushed = false;
    bool preview_window_ready = false;
};

struct DecodeExactSeekReorderCallbacks {
    std::function<void()> drain_codec;
    std::function<size_t()> reorder_count_after_drain;
    std::function<void(size_t)> log_after_drain;
    std::function<void()> publish_best_frame;
    std::function<std::optional<int64_t>()> first_published_pts_us;
    std::function<void(std::optional<int64_t>)> log_after_publish;
};

struct DecodeExactSeekReorderResult {
    bool drained_codec = false;
    bool published = false;
};

DecodeExactSeekReorderResult handle_exact_seek_reorder_after_receive(
    const DecodeExactSeekReorderState& state,
    const DecodeExactSeekReorderCallbacks& callbacks);

} // namespace vr
