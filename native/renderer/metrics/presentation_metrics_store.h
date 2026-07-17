#pragma once

#include "renderer/render/presentation_backend_types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace vr {

// Lock contract:
// - Owns presentation metric atomics and draw timing sample storage.
// - Does not take renderer state/device/texture locks.
// - Callers pass layout revisions as values when building public snapshots.
class PresentationMetricsStore {
public:
    void reset();
    // total_us is the end-to-end presentation lifecycle. callback_us is the
    // host callback portion (for example a blocking compositor Present), and
    // backend_blocking_wait_us is explicit GPU/pacing synchronization. Work is
    // derived without either wait so policy measures submission work only.
    void record_draw_timing(uint64_t total_us,
                            uint64_t backend_us,
                            uint64_t callback_us,
                            uint64_t backend_blocking_wait_us);
    void note_presentation_target_resize();
    void note_layout_presented();
    void note_layout_intent();
    void note_layout_deferred_to_playback();
    void note_layout_stale_completion_drop();
    void note_device_lost();
    uint64_t note_playing_layout_redraw_suppressed();
    void note_present_publish(uint64_t elapsed_us);
    uint64_t note_transient_backpressure(std::chrono::microseconds backoff,
                                         int64_t now_us);
    std::chrono::microseconds transient_backpressure_remaining(int64_t now_us) const;
    PresentationBackendMetrics snapshot(uint64_t last_layout_revision,
                                        uint64_t last_presented_layout_revision) const;

    std::atomic<uint64_t> render_wait_us{0};
    std::atomic<uint64_t> render_wait_count{0};
    std::atomic<uint64_t> frame_copy_us{0};
    std::atomic<uint64_t> frame_copy_count{0};
    std::atomic<uint64_t> present_publish_us{0};
    std::atomic<uint64_t> present_publish_count{0};
    std::atomic<uint64_t> presentation_target_resize_count{0};
    std::atomic<uint64_t> device_lost_count{0};
    std::atomic<uint64_t> layout_intent_count{0};
    std::atomic<uint64_t> layout_presented_count{0};
    std::atomic<uint64_t> layout_deferred_to_playback_count{0};
    std::atomic<uint64_t> playing_layout_redraw_suppressed_count{0};
    std::atomic<uint64_t> layout_stale_completion_drop_count{0};

private:
    std::atomic<uint64_t> draw_count_{0};
    std::atomic<uint64_t> draw_total_us_{0};
    std::atomic<uint64_t> draw_max_us_{0};
    std::atomic<uint64_t> draw_backend_total_us_{0};
    std::atomic<uint64_t> draw_backend_max_us_{0};
    std::atomic<uint64_t> draw_p95_us_{0};
    std::atomic<uint64_t> draw_work_total_us_{0};
    std::atomic<uint64_t> draw_work_max_us_{0};
    std::atomic<uint64_t> draw_work_p95_us_{0};
    std::atomic<uint64_t> draw_callback_total_us_{0};
    std::atomic<uint64_t> draw_callback_max_us_{0};
    std::atomic<uint64_t> draw_callback_p95_us_{0};
    std::atomic<uint64_t> draw_blocking_wait_total_us_{0};
    std::atomic<uint64_t> draw_blocking_wait_max_us_{0};
    std::atomic<uint64_t> draw_blocking_wait_p95_us_{0};
    std::atomic<uint64_t> draw_backend_p95_us_{0};
    std::atomic<uint64_t> draw_backend_work_total_us_{0};
    std::atomic<uint64_t> draw_backend_work_max_us_{0};
    std::atomic<uint64_t> draw_backend_work_p95_us_{0};
    std::atomic<uint64_t> transient_backpressure_skip_count_{0};
    std::atomic<int64_t> transient_backpressure_until_us_{0};
    mutable std::mutex draw_samples_mutex_;
    std::vector<uint64_t> draw_total_samples_us_;
    std::vector<uint64_t> draw_work_samples_us_;
    std::vector<uint64_t> draw_callback_samples_us_;
    std::vector<uint64_t> draw_blocking_wait_samples_us_;
    std::vector<uint64_t> draw_backend_samples_us_;
    std::vector<uint64_t> draw_backend_work_samples_us_;
};

} // namespace vr
