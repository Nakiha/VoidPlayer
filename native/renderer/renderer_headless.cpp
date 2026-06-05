#include "renderer/renderer_internal.h"
#include "renderer/render/renderer_draw_snapshot_builder.h"

namespace vr {

void Renderer::Impl::set_frame_callback(RendererFrameCallback cb) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
#ifdef _WIN32
    if (presentation_.set_d3d_headless_frame_callback(cb)) {
        return;
    }
#endif
    presentation_.set_frame_callback(std::move(cb));
}

void Renderer::Impl::set_frame_failure_callback(std::function<void(const char*)> cb) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    presentation_.set_frame_failure_callback(std::move(cb));
}

void Renderer::Impl::set_event_callback(RendererEventCallback cb) {
    event_bus_.set_callback(std::move(cb));
}

bool Renderer::Impl::acquire_shared_texture(SharedTextureSnapshot& snapshot) const {
    snapshot = {};

#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return presentation_.acquire_d3d_shared_texture(
        snapshot, presentation_metrics_);
#else
    return false;
#endif
}

void Renderer::Impl::release_shared_texture(int buffer_index, uint64_t buffer_generation) const {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    presentation_.release_d3d_shared_texture(buffer_index, buffer_generation);
#else
    (void)buffer_index;
    (void)buffer_generation;
#endif
}

void Renderer::Impl::resize(int width, int height) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
#ifdef _WIN32
    if (!surface_state_.headless() || !presentation_.d3d_device()) return;
#else
    if (!surface_state_.headless() || !presentation_.has_backend()) return;
#endif
    const auto validation = validate_renderer_dimensions(width, height, "resize dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid resize: {}", validation.message);
        return;
    }
    loop_driver_.request_resize(width, height);
}

bool Renderer::Impl::update_headless_output(void* output,
                                      int width,
                                      int height,
                                      int max_track_slots) {
    RendererFrameCallback frame_callback;
    auto frame_failure_callback = presentation_.frame_failure_callback_snapshot();
    std::string frame_failure_error;
    bool drew = false;
    bool attempted_draw = false;
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!surface_state_.headless() || !presentation_.has_backend()) {
            return false;
        }
        const auto validation =
            validate_renderer_dimensions(width, height, "headless output dimensions");
        if (!validation.ok) {
            spdlog::warn("[Renderer] ignoring invalid headless output: {}",
                         validation.message);
            return false;
        }
        if (!presentation_.update_headless_output(
                output, width, height, max_track_slots)) {
            return false;
        }

        RendererDrawSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto resize = surface_state_.resize_if_changed(width, height);
            if (resize.changed) {
                const auto layout_tracks = track_controller_.layout_track_geometry();
                layout_state_.adjust_view_offset_for_resize(
                    resize.old_width,
                    resize.old_height,
                    resize.width,
                    resize.height,
                    layout_tracks);
            }
            loop_driver_.force_preview_redraw();
            snapshot = RendererDrawSnapshotBuilder::build(
                track_controller_,
                layout_state_,
                surface_state_,
                present_history_.snapshot());
        }
        if (present_decision_has_frame(snapshot.decision)) {
            attempted_draw = true;
            std::lock_guard<std::recursive_mutex> ctx_lock(presentation_.device_mutex());
            drew = presentation_.draw_frame(snapshot,
                                            "install_headless_output",
                                            presentation_metrics_,
                                            presentation_overlay_hooks());
            if (drew) {
                frame_callback = presentation_.frame_callback_snapshot();
            } else {
                frame_failure_error = presentation_.backend_last_error();
            }
        }
    }
    if (drew) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (layout_state_.mark_presented_if_newer(layout_state_.current_revision())) {
            presentation_metrics_.note_layout_presented();
        }
        loop_driver_.mark_preview_presented(true);
    }
    if (frame_callback && !shutting_down_.load(std::memory_order_acquire)) {
        frame_callback(nullptr);
    }
    if (attempted_draw && !drew && frame_failure_callback &&
        !shutting_down_.load(std::memory_order_acquire)) {
        frame_failure_callback(frame_failure_error.c_str());
    }
    return true;
}

bool Renderer::Impl::install_headless_output(void* output,
                                       int width,
                                       int height,
                                       int max_track_slots) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.headless() || !presentation_.has_backend()) {
        return false;
    }
    const auto validation =
        validate_renderer_dimensions(width, height, "headless output dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid headless output: {}",
                     validation.message);
        return false;
    }
    if (!presentation_.update_headless_output(
            output, width, height, max_track_slots)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto resize = surface_state_.resize_if_changed(width, height);
        if (resize.changed) {
            const auto layout_tracks = track_controller_.layout_track_geometry();
            layout_state_.adjust_view_offset_for_resize(
                resize.old_width,
                resize.old_height,
                resize.width,
                resize.height,
                layout_tracks);
        }
        loop_driver_.force_preview_redraw();
    }
    return true;
}

bool Renderer::Impl::install_headless_output_ring(const void* const* pixel_buffers,
                                            size_t pixel_buffer_count,
                                            void* displayed_pixel_buffer,
                                            void* protected_pixel_buffer,
                                            int width,
                                            int height,
                                            int max_track_slots) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.headless() || !presentation_.has_backend()) {
        return false;
    }
    const auto validation =
        validate_renderer_dimensions(width, height, "headless output dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid headless output ring: {}",
                     validation.message);
        return false;
    }
    if (!presentation_.update_headless_output_ring(pixel_buffers,
                                                   pixel_buffer_count,
                                                   displayed_pixel_buffer,
                                                   protected_pixel_buffer,
                                                   width,
                                                   height,
                                                   max_track_slots)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto resize = surface_state_.resize_if_changed(width, height);
        if (resize.changed) {
            const auto layout_tracks = track_controller_.layout_track_geometry();
            layout_state_.adjust_view_offset_for_resize(
                resize.old_width,
                resize.old_height,
                resize.width,
                resize.height,
                layout_tracks);
        }
        loop_driver_.force_preview_redraw();
    }
    return true;
}

void Renderer::Impl::mark_headless_output_displayed(void* pixel_buffer) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.headless() || !presentation_.has_backend()) {
        return;
    }
    presentation_.mark_headless_output_displayed(pixel_buffer);
}

void Renderer::Impl::protect_headless_output(void* pixel_buffer) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.headless() || !presentation_.has_backend()) {
        return;
    }
    presentation_.protect_headless_output(pixel_buffer);
}

void Renderer::Impl::release_headless_output(void* pixel_buffer) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.headless() || !presentation_.has_backend()) {
        return;
    }
    presentation_.release_headless_output(pixel_buffer);
}

void Renderer::Impl::clear_headless_output() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.headless() || !presentation_.has_backend()) {
        return;
    }
    presentation_.clear_headless_output();
}

} // namespace vr
