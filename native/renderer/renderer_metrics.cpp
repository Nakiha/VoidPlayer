#include "renderer/renderer_internal.h"

namespace vr {

std::string Renderer::Impl::presentation_backend_last_error() const {
    return presentation_.backend_last_error();
}

PresentationBackendMetrics Renderer::Impl::presentation_backend_metrics() const {
    uint64_t layout_revision = 0;
    uint64_t last_presented_layout_revision = 0;
    {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        layout_revision = layout_state_.current_revision();
        last_presented_layout_revision = layout_state_.last_presented_revision();
    }
    return presentation_metrics_.snapshot(layout_revision, last_presented_layout_revision);
}

D3D11BackendMetrics Renderer::Impl::d3d_backend_metrics() const {
    return presentation_backend_metrics();
}

PresentationBackendStats Renderer::Impl::presentation_backend_stats() const {
    return presentation_.backend_stats();
}

bool Renderer::Impl::copy_last_presentation_frame_info(
    PresentationBackendFrameInfo* out) const {
    return presentation_.copy_last_frame_info(out);
}

RendererGpuMemoryStats Renderer::Impl::gpu_memory_stats() const {
    auto presentation_memory = presentation_.d3d_memory_snapshot();
    RendererGpuMemoryStats result = std::move(presentation_memory.stats);
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto track_memory =
        track_controller_.gpu_memory_stats(
            presentation_memory.presenter_copy_texture_bytes_by_slot);
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

bool Renderer::Impl::d3d_device_lost() const {
    return device_state_.load(std::memory_order_acquire) != RendererDeviceState::Ready ||
           presentation_.device_lost();
}

long Renderer::Impl::d3d_device_removed_reason() const {
    return presentation_.device_removed_reason();
}

RendererDeviceState Renderer::Impl::device_state() const {
    return device_state_.load(std::memory_order_acquire);
}

} // namespace vr
