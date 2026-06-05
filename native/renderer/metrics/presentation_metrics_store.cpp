#include "renderer/metrics/presentation_metrics_store.h"

#include <algorithm>
#include <cmath>

namespace vr {

namespace {

void atomic_fetch_max(std::atomic<uint64_t>& target, uint64_t value) {
    auto current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(
               current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

uint64_t percentile_95_us(std::vector<uint64_t> samples) {
    if (samples.empty()) {
        return 0;
    }
    std::sort(samples.begin(), samples.end());
    const auto index = std::min(
        samples.size() - 1,
        static_cast<size_t>(std::ceil(static_cast<double>(samples.size()) * 0.95) - 1.0));
    return samples[index];
}

} // namespace

void PresentationMetricsStore::reset() {
    draw_count_.store(0, std::memory_order_relaxed);
    draw_total_us_.store(0, std::memory_order_relaxed);
    draw_max_us_.store(0, std::memory_order_relaxed);
    draw_backend_total_us_.store(0, std::memory_order_relaxed);
    draw_backend_max_us_.store(0, std::memory_order_relaxed);
    draw_p95_us_.store(0, std::memory_order_relaxed);
    draw_backend_p95_us_.store(0, std::memory_order_relaxed);
    render_wait_us.store(0, std::memory_order_relaxed);
    render_wait_count.store(0, std::memory_order_relaxed);
    frame_copy_us.store(0, std::memory_order_relaxed);
    frame_copy_count.store(0, std::memory_order_relaxed);
    present_publish_us.store(0, std::memory_order_relaxed);
    present_publish_count.store(0, std::memory_order_relaxed);
    shared_texture_resize_count.store(0, std::memory_order_relaxed);
    device_lost_count.store(0, std::memory_order_relaxed);
    texture_sharing_failure_count.store(0, std::memory_order_relaxed);
    layout_intent_count.store(0, std::memory_order_relaxed);
    layout_presented_count.store(0, std::memory_order_relaxed);
    layout_deferred_to_playback_count.store(0, std::memory_order_relaxed);
    playing_layout_redraw_suppressed_count.store(0, std::memory_order_relaxed);
    layout_stale_completion_drop_count.store(0, std::memory_order_relaxed);
    transient_backpressure_skip_count_.store(0, std::memory_order_relaxed);
    transient_backpressure_until_us_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(draw_samples_mutex_);
        draw_total_samples_us_.clear();
        draw_backend_samples_us_.clear();
    }
}

void PresentationMetricsStore::record_draw_timing(uint64_t total_us, uint64_t backend_us) {
    const auto draw_count = draw_count_.fetch_add(1, std::memory_order_relaxed) + 1;
    draw_total_us_.fetch_add(total_us, std::memory_order_relaxed);
    draw_backend_total_us_.fetch_add(backend_us, std::memory_order_relaxed);
    atomic_fetch_max(draw_max_us_, total_us);
    atomic_fetch_max(draw_backend_max_us_, backend_us);
    {
        std::lock_guard<std::mutex> lock(draw_samples_mutex_);
        draw_total_samples_us_.push_back(total_us);
        draw_backend_samples_us_.push_back(backend_us);
        static constexpr size_t kMaxDrawTimingSamples = 240;
        if (draw_total_samples_us_.size() > kMaxDrawTimingSamples) {
            const auto remove_count = draw_total_samples_us_.size() - kMaxDrawTimingSamples;
            draw_total_samples_us_.erase(
                draw_total_samples_us_.begin(),
                draw_total_samples_us_.begin() +
                    static_cast<std::vector<uint64_t>::difference_type>(remove_count));
        }
        if (draw_backend_samples_us_.size() > kMaxDrawTimingSamples) {
            const auto remove_count = draw_backend_samples_us_.size() - kMaxDrawTimingSamples;
            draw_backend_samples_us_.erase(
                draw_backend_samples_us_.begin(),
                draw_backend_samples_us_.begin() +
                    static_cast<std::vector<uint64_t>::difference_type>(remove_count));
        }
        if (draw_count <= 16 || (draw_count % 16) == 0) {
            draw_p95_us_.store(percentile_95_us(draw_total_samples_us_),
                               std::memory_order_relaxed);
            draw_backend_p95_us_.store(percentile_95_us(draw_backend_samples_us_),
                                       std::memory_order_relaxed);
        }
    }
}

void PresentationMetricsStore::note_shared_texture_resize() {
    shared_texture_resize_count.fetch_add(1, std::memory_order_relaxed);
}

void PresentationMetricsStore::note_layout_presented() {
    layout_presented_count.fetch_add(1, std::memory_order_relaxed);
}

void PresentationMetricsStore::note_layout_intent() {
    layout_intent_count.fetch_add(1, std::memory_order_relaxed);
}

void PresentationMetricsStore::note_layout_deferred_to_playback() {
    layout_deferred_to_playback_count.fetch_add(1, std::memory_order_relaxed);
}

void PresentationMetricsStore::note_layout_stale_completion_drop() {
    layout_stale_completion_drop_count.fetch_add(1, std::memory_order_relaxed);
}

void PresentationMetricsStore::note_device_lost() {
    device_lost_count.fetch_add(1, std::memory_order_relaxed);
}

void PresentationMetricsStore::note_texture_sharing_failure() {
    texture_sharing_failure_count.fetch_add(1, std::memory_order_relaxed);
}

uint64_t PresentationMetricsStore::note_playing_layout_redraw_suppressed() {
    return playing_layout_redraw_suppressed_count.fetch_add(
        1, std::memory_order_relaxed) + 1;
}

void PresentationMetricsStore::note_present_publish(uint64_t elapsed_us) {
    present_publish_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    present_publish_count.fetch_add(1, std::memory_order_relaxed);
}

uint64_t PresentationMetricsStore::note_transient_backpressure(
    std::chrono::microseconds backoff,
    int64_t now_us) {
    const int64_t until_us = now_us + static_cast<int64_t>(backoff.count());
    auto current = transient_backpressure_until_us_.load(std::memory_order_relaxed);
    while (until_us > current &&
           !transient_backpressure_until_us_.compare_exchange_weak(
               current,
               until_us,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    return transient_backpressure_skip_count_.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::chrono::microseconds
PresentationMetricsStore::transient_backpressure_remaining(int64_t now_us) const {
    const int64_t until_us = transient_backpressure_until_us_.load(std::memory_order_relaxed);
    const int64_t remaining_us = until_us - now_us;
    if (remaining_us <= 0) {
        return std::chrono::microseconds(0);
    }
    return std::chrono::microseconds(remaining_us);
}

PresentationBackendMetrics PresentationMetricsStore::snapshot(
    uint64_t last_layout_revision,
    uint64_t last_presented_layout_revision) const {
    PresentationBackendMetrics result;
    result.draw_count = draw_count_.load(std::memory_order_relaxed);
    result.draw_total_us = draw_total_us_.load(std::memory_order_relaxed);
    result.draw_max_us = draw_max_us_.load(std::memory_order_relaxed);
    result.draw_backend_total_us = draw_backend_total_us_.load(std::memory_order_relaxed);
    result.draw_backend_max_us = draw_backend_max_us_.load(std::memory_order_relaxed);
    result.render_wait_us = render_wait_us.load(std::memory_order_relaxed);
    result.render_wait_count = render_wait_count.load(std::memory_order_relaxed);
    result.frame_copy_us = frame_copy_us.load(std::memory_order_relaxed);
    result.frame_copy_count = frame_copy_count.load(std::memory_order_relaxed);
    result.present_publish_us = present_publish_us.load(std::memory_order_relaxed);
    result.present_publish_count = present_publish_count.load(std::memory_order_relaxed);
    result.shared_texture_resize_count =
        shared_texture_resize_count.load(std::memory_order_relaxed);
    result.device_lost_count = device_lost_count.load(std::memory_order_relaxed);
    result.texture_sharing_failure_count =
        texture_sharing_failure_count.load(std::memory_order_relaxed);
    result.layout_intent_count = layout_intent_count.load(std::memory_order_relaxed);
    result.layout_presented_count = layout_presented_count.load(std::memory_order_relaxed);
    result.layout_deferred_to_playback_count =
        layout_deferred_to_playback_count.load(std::memory_order_relaxed);
    result.playing_layout_redraw_suppressed_count =
        playing_layout_redraw_suppressed_count.load(std::memory_order_relaxed);
    result.layout_stale_completion_drop_count =
        layout_stale_completion_drop_count.load(std::memory_order_relaxed);
    result.last_layout_revision = last_layout_revision;
    result.last_presented_layout_revision = last_presented_layout_revision;
    result.draw_p95_us = draw_p95_us_.load(std::memory_order_relaxed);
    result.draw_backend_p95_us = draw_backend_p95_us_.load(std::memory_order_relaxed);
    return result;
}

} // namespace vr
