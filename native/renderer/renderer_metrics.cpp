#include "renderer/renderer_internal.h"

namespace vr {

void Renderer::reset_presentation_backend_metrics() {
    presentation_backend_metrics_.draw_count.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.draw_total_us.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.draw_max_us.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.draw_backend_total_us.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.draw_backend_max_us.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.draw_p95_us.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.draw_backend_p95_us.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.render_wait_us.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.render_wait_count.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.frame_copy_us.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.frame_copy_count.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.present_publish_us.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.present_publish_count.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.shared_texture_resize_count.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.device_lost_count.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.texture_sharing_failure_count.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.layout_intent_count.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.layout_presented_count.store(0, std::memory_order_relaxed);
    presentation_backend_metrics_.layout_deferred_to_playback_count.store(
        0, std::memory_order_relaxed);
    presentation_backend_metrics_.playing_layout_redraw_suppressed_count.store(
        0, std::memory_order_relaxed);
    presentation_backend_metrics_.layout_stale_completion_drop_count.store(
        0, std::memory_order_relaxed);
    presentation_backend_metrics_.transient_backpressure_skip_count.store(
        0, std::memory_order_relaxed);
    transient_presentation_backpressure_until_us_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(presentation_draw_samples_mutex_);
        presentation_draw_total_samples_us_.clear();
        presentation_draw_backend_samples_us_.clear();
    }
}

void Renderer::record_presentation_draw_timing(uint64_t total_us, uint64_t backend_us) {
    const auto draw_count =
        presentation_backend_metrics_.draw_count.fetch_add(1, std::memory_order_relaxed) + 1;
    presentation_backend_metrics_.draw_total_us.fetch_add(total_us, std::memory_order_relaxed);
    presentation_backend_metrics_.draw_backend_total_us.fetch_add(
        backend_us, std::memory_order_relaxed);
    atomic_fetch_max(presentation_backend_metrics_.draw_max_us, total_us);
    atomic_fetch_max(presentation_backend_metrics_.draw_backend_max_us, backend_us);
    {
        std::lock_guard<std::mutex> lock(presentation_draw_samples_mutex_);
        presentation_draw_total_samples_us_.push_back(total_us);
        presentation_draw_backend_samples_us_.push_back(backend_us);
        static constexpr size_t kMaxDrawTimingSamples = 240;
        if (presentation_draw_total_samples_us_.size() > kMaxDrawTimingSamples) {
            const auto remove_count =
                presentation_draw_total_samples_us_.size() - kMaxDrawTimingSamples;
            presentation_draw_total_samples_us_.erase(
                presentation_draw_total_samples_us_.begin(),
                presentation_draw_total_samples_us_.begin() +
                    static_cast<std::vector<uint64_t>::difference_type>(remove_count));
        }
        if (presentation_draw_backend_samples_us_.size() > kMaxDrawTimingSamples) {
            const auto remove_count =
                presentation_draw_backend_samples_us_.size() - kMaxDrawTimingSamples;
            presentation_draw_backend_samples_us_.erase(
                presentation_draw_backend_samples_us_.begin(),
                presentation_draw_backend_samples_us_.begin() +
                    static_cast<std::vector<uint64_t>::difference_type>(remove_count));
        }
        if (draw_count <= 16 || (draw_count % 16) == 0) {
            presentation_backend_metrics_.draw_p95_us.store(
                percentile_95_us(presentation_draw_total_samples_us_),
                std::memory_order_relaxed);
            presentation_backend_metrics_.draw_backend_p95_us.store(
                percentile_95_us(presentation_draw_backend_samples_us_),
                std::memory_order_relaxed);
        }
    }
}

void Renderer::note_transient_presentation_backpressure(const char* source) {
    const int64_t until_us =
        steady_clock_us_now() +
        static_cast<int64_t>(kTransientPresentationBackpressureBackoff.count());
    auto current = transient_presentation_backpressure_until_us_.load(
        std::memory_order_relaxed);
    while (until_us > current &&
           !transient_presentation_backpressure_until_us_.compare_exchange_weak(
               current,
               until_us,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    const auto count = presentation_backend_metrics_.transient_backpressure_skip_count
                           .fetch_add(1, std::memory_order_relaxed) +
                       1;
    if (profiler_enabled("VOIDPLAYER_MACOS_PROFILER") &&
        (count <= 8 || count % 120 == 0)) {
        spdlog::info(
            "[RendererProfiler] transient presentation backpressure source={} "
            "backoff_us={} count={}",
            source ? source : "",
            kTransientPresentationBackpressureBackoff.count(),
            count);
    }
}

std::chrono::microseconds
Renderer::transient_presentation_backpressure_remaining() const {
    const int64_t until_us = transient_presentation_backpressure_until_us_.load(
        std::memory_order_relaxed);
    const int64_t remaining_us = until_us - steady_clock_us_now();
    if (remaining_us <= 0) {
        return std::chrono::microseconds(0);
    }
    return std::chrono::microseconds(remaining_us);
}

std::function<void(const char*)> Renderer::frame_failure_callback_snapshot() const {
    return frame_failure_callback_;
}

std::string Renderer::presentation_backend_last_error() const {
    if (!presentation_backend_) {
        return "presentation backend is not available";
    }
    const char* error = presentation_backend_->last_error();
    return error && error[0] != '\0' ? error : "presentation backend draw failed";
}

PresentationBackendMetrics Renderer::presentation_backend_metrics() const {
    PresentationBackendMetrics result;
    result.draw_count =
        presentation_backend_metrics_.draw_count.load(std::memory_order_relaxed);
    result.draw_total_us =
        presentation_backend_metrics_.draw_total_us.load(std::memory_order_relaxed);
    result.draw_max_us =
        presentation_backend_metrics_.draw_max_us.load(std::memory_order_relaxed);
    result.draw_backend_total_us =
        presentation_backend_metrics_.draw_backend_total_us.load(std::memory_order_relaxed);
    result.draw_backend_max_us =
        presentation_backend_metrics_.draw_backend_max_us.load(std::memory_order_relaxed);
    result.render_wait_us =
        presentation_backend_metrics_.render_wait_us.load(std::memory_order_relaxed);
    result.render_wait_count =
        presentation_backend_metrics_.render_wait_count.load(std::memory_order_relaxed);
    result.frame_copy_us =
        presentation_backend_metrics_.frame_copy_us.load(std::memory_order_relaxed);
    result.frame_copy_count =
        presentation_backend_metrics_.frame_copy_count.load(std::memory_order_relaxed);
    result.present_publish_us =
        presentation_backend_metrics_.present_publish_us.load(std::memory_order_relaxed);
    result.present_publish_count =
        presentation_backend_metrics_.present_publish_count.load(std::memory_order_relaxed);
    result.shared_texture_resize_count =
        presentation_backend_metrics_.shared_texture_resize_count.load(std::memory_order_relaxed);
    result.device_lost_count =
        presentation_backend_metrics_.device_lost_count.load(std::memory_order_relaxed);
    result.texture_sharing_failure_count =
        presentation_backend_metrics_.texture_sharing_failure_count.load(std::memory_order_relaxed);
    result.layout_intent_count =
        presentation_backend_metrics_.layout_intent_count.load(std::memory_order_relaxed);
    result.layout_presented_count =
        presentation_backend_metrics_.layout_presented_count.load(std::memory_order_relaxed);
    result.layout_deferred_to_playback_count =
        presentation_backend_metrics_.layout_deferred_to_playback_count.load(
            std::memory_order_relaxed);
    result.playing_layout_redraw_suppressed_count =
        presentation_backend_metrics_.playing_layout_redraw_suppressed_count.load(
            std::memory_order_relaxed);
    result.layout_stale_completion_drop_count =
        presentation_backend_metrics_.layout_stale_completion_drop_count.load(
            std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        result.last_layout_revision = layout_revision_;
        result.last_presented_layout_revision = last_presented_layout_revision_;
    }
    result.draw_p95_us =
        presentation_backend_metrics_.draw_p95_us.load(std::memory_order_relaxed);
    result.draw_backend_p95_us =
        presentation_backend_metrics_.draw_backend_p95_us.load(std::memory_order_relaxed);
    return result;
}

D3D11BackendMetrics Renderer::d3d_backend_metrics() const {
    return presentation_backend_metrics();
}

PresentationBackendStats Renderer::presentation_backend_stats() const {
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    if (!presentation_backend_) {
        return {};
    }
    return presentation_backend_->presentation_stats();
}

bool Renderer::copy_last_presentation_frame_info(
    PresentationBackendFrameInfo* out) const {
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    return presentation_backend_ && presentation_backend_->copy_last_frame_info(out);
}

RendererGpuMemoryStats Renderer::gpu_memory_stats() const {
    RendererGpuMemoryStats result;

#ifdef _WIN32
    D3D11FramePresenterMemoryStats presenter_stats{};
    {
        std::lock_guard<std::recursive_mutex> device_lock(device_mutex_);
        auto* presenter = frame_presenter();
        presenter_stats = presenter
            ? presenter->memory_stats()
            : D3D11FramePresenterMemoryStats{};
        result.presenter_texture_bytes = presenter_stats.total_estimated_bytes;
        result.total_estimated_bytes += result.presenter_texture_bytes;

        if (auto* output = headless_output()) {
            const auto headless_stats = output->memory_stats();
            result.headless_output_bytes = headless_stats.estimated_bytes;
            result.headless_width = headless_stats.width;
            result.headless_height = headless_stats.height;
            result.headless_buffer_count = headless_stats.buffer_count;
            result.total_estimated_bytes += result.headless_output_bytes;
        }

        if (auto* resources = d3d_resources()) {
            const auto overlay_stats =
                snapshot_analysis_overlay_memory_stats(*resources);
            result.analysis_overlay_bytes = overlay_stats.estimated_bytes;
            result.analysis_overlay_width = overlay_stats.width;
            result.analysis_overlay_height = overlay_stats.height;
            if (result.analysis_overlay_bytes > 0) {
                result.total_estimated_bytes += result.analysis_overlay_bytes;
            }
        }
    }

    std::array<uint64_t, kMaxTracks> presenter_copy_texture_bytes_by_slot{};
    for (size_t i = 0; i < kMaxTracks && i < presenter_stats.slots.size(); ++i) {
        presenter_copy_texture_bytes_by_slot[i] =
            presenter_stats.slots[i].render_nv12_copy_texture_bytes;
    }
#else
    std::array<uint64_t, kMaxTracks> presenter_copy_texture_bytes_by_slot{};
#endif
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto track_memory = snapshot_track_gpu_memory_stats_collection(
        tracks_, presenter_copy_texture_bytes_by_slot);
    result.decoder_pool_bytes += track_memory.decoder_pool_bytes;
    result.exact_seek_snapshot_bytes += track_memory.exact_seek_snapshot_bytes;
    result.track_buffer_cpu_bytes += track_memory.track_buffer_cpu_bytes;
    result.packet_queue_bytes += track_memory.packet_queue_bytes;
    result.exact_seek_candidate_cpu_bytes +=
        track_memory.exact_seek_candidate_cpu_bytes;
    result.exact_seek_stable_cpu_bytes += track_memory.exact_seek_stable_cpu_bytes;
    result.exact_seek_budget_drop_count += track_memory.exact_seek_budget_drop_count;
    result.cpu_frame_bytes += track_memory.cpu_frame_bytes;
    result.total_estimated_bytes += track_memory.total_estimated_bytes;
    result.tracks = track_memory.tracks;

    return result;
}

bool Renderer::d3d_device_lost() const {
    return device_state_.load(std::memory_order_acquire) != RendererDeviceState::Ready ||
           (presentation_backend_ && presentation_backend_->device_lost());
}

long Renderer::d3d_device_removed_reason() const {
    return presentation_backend_ ? presentation_backend_->device_removed_reason() : 0;
}

RendererDeviceState Renderer::device_state() const {
    return device_state_.load(std::memory_order_acquire);
}

} // namespace vr
