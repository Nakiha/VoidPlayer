#include "renderer/decode/decode_exact_seek_reorder.h"

#include "renderer/decode/decode_loop_policy.h"

namespace vr {

DecodeExactSeekReorderResult handle_exact_seek_reorder_after_receive(
    const DecodeExactSeekReorderState& state,
    const DecodeExactSeekReorderCallbacks& callbacks) {
    DecodeExactSeekReorderResult result;
    const auto decision = choose_exact_seek_reorder_publish(
        state.exact_seek_active,
        state.reorder_count,
        state.queue_eof,
        state.queue_size,
        state.eof_flushed,
        state.preview_window_ready);

    if (decision.drain_codec) {
        if (callbacks.drain_codec) {
            callbacks.drain_codec();
        }
        result.drained_codec = true;
        if (callbacks.log_after_drain) {
            const size_t count_after_drain = callbacks.reorder_count_after_drain
                ? callbacks.reorder_count_after_drain()
                : state.reorder_count;
            callbacks.log_after_drain(count_after_drain);
        }
    }

    if (decision.publish) {
        if (callbacks.publish_best_frame) {
            callbacks.publish_best_frame();
        }
        result.published = true;
        if (callbacks.log_after_publish) {
            callbacks.log_after_publish(
                callbacks.first_published_pts_us
                    ? callbacks.first_published_pts_us()
                    : std::optional<int64_t>{});
        }
    }

    return result;
}

} // namespace vr
