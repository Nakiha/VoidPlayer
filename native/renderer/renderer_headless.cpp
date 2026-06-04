#include "renderer/renderer_internal.h"

namespace vr {

void Renderer::set_frame_callback(RendererFrameCallback cb) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
#ifdef _WIN32
    if (auto* output = headless_output()) {
        output->set_frame_callback([cb = std::move(cb)]() {
            if (cb) {
                cb(nullptr);
            }
        });
        frame_callback_ = {};
        return;
    }
#endif
    frame_callback_ = std::move(cb);
}

void Renderer::set_frame_failure_callback(std::function<void(const char*)> cb) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    frame_failure_callback_ = std::move(cb);
}

void Renderer::set_event_callback(RendererEventCallback cb) {
    std::lock_guard<std::mutex> lock(event_callback_mutex_);
    event_callback_ = std::move(cb);
}

bool Renderer::acquire_shared_texture(SharedTextureSnapshot& snapshot) const {
    snapshot = {};

#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    auto* output = headless_output();
    if (!output) {
        presentation_backend_metrics_.texture_sharing_failure_count.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }

    std::lock_guard<std::mutex> lock(texture_mutex());
    D3D11HeadlessOutputTextureLease lease;
    if (!output->acquire_shared_texture_locked(lease)) {
        presentation_backend_metrics_.texture_sharing_failure_count.fetch_add(
            1, std::memory_order_relaxed);
        return false;
    }

    snapshot.type = SharedTextureHandleType::D3D11SharedHandle;
    snapshot.texture = lease.texture;
    snapshot.handle = lease.handle;
    snapshot.width = lease.width;
    snapshot.height = lease.height;
    snapshot.buffer_index = lease.buffer_index;
    snapshot.buffer_generation = lease.generation;
    return true;
#else
    return false;
#endif
}

void Renderer::release_shared_texture(int buffer_index, uint64_t buffer_generation) const {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (auto* output = headless_output()) {
        output->release_shared_texture(buffer_index, buffer_generation);
    }
#else
    (void)buffer_index;
    (void)buffer_generation;
#endif
}

std::mutex& Renderer::texture_mutex() const {
#ifdef _WIN32
    auto* output = headless_output();
    return output ? output->texture_mutex() : texture_mutex_fallback_;
#else
    return texture_mutex_fallback_;
#endif
}

void Renderer::resize(int width, int height) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
#ifdef _WIN32
    if (!headless_ || !d3d_device()) return;
#else
    if (!headless_ || !presentation_backend_) return;
#endif
    const auto validation = validate_renderer_dimensions(width, height, "resize dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid resize: {}", validation.message);
        return;
    }
    pending_width_.store(width);
    pending_height_.store(height);
}

bool Renderer::update_headless_output(void* output,
                                      int width,
                                      int height,
                                      int max_track_slots) {
    RendererFrameCallback frame_callback;
    auto frame_failure_callback = frame_failure_callback_snapshot();
    std::string frame_failure_error;
    bool drew = false;
    bool attempted_draw = false;
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!headless_ || !presentation_backend_) {
            return false;
        }
        const auto validation =
            validate_renderer_dimensions(width, height, "headless output dimensions");
        if (!validation.ok) {
            spdlog::warn("[Renderer] ignoring invalid headless output: {}",
                         validation.message);
            return false;
        }
        {
            std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
            if (!presentation_backend_->update_headless_output(
                    output, width, height, max_track_slots)) {
                return false;
            }
        }

        RendererDrawSnapshot snapshot;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            const auto old_width = target_width_;
            const auto old_height = target_height_;
            if (old_width != width || old_height != height) {
                const auto layout_tracks = snapshot_layout_track_geometry(tracks_);
                adjust_layout_view_offset_for_resize(
                    layout_, old_width, old_height, width, height, layout_tracks);
                target_width_ = width;
                target_height_ = height;
            }
            snapshot = build_draw_snapshot_locked(last_decision_);
        }
        if (present_decision_has_frame(snapshot.decision)) {
            attempted_draw = true;
            std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
            drew = draw_frame(snapshot, "install_headless_output");
            if (drew) {
                frame_callback = frame_callback_;
            } else {
                frame_failure_error = presentation_backend_last_error();
            }
        }
    }
    if (drew) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (layout_revision_ > last_presented_layout_revision_) {
            last_presented_layout_revision_ = layout_revision_;
            presentation_backend_metrics_.layout_presented_count.fetch_add(
                1, std::memory_order_relaxed);
        }
        preview_drawn_ = true;
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

bool Renderer::install_headless_output(void* output,
                                       int width,
                                       int height,
                                       int max_track_slots) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_ || !presentation_backend_) {
        return false;
    }
    const auto validation =
        validate_renderer_dimensions(width, height, "headless output dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid headless output: {}",
                     validation.message);
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        if (!presentation_backend_->update_headless_output(
                output, width, height, max_track_slots)) {
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (target_width_ == width && target_height_ == height) {
            return true;
        }
        const auto layout_tracks = snapshot_layout_track_geometry(tracks_);
        adjust_layout_view_offset_for_resize(
            layout_, target_width_, target_height_, width, height, layout_tracks);
        target_width_ = width;
        target_height_ = height;
        preview_drawn_ = false;
    }
    return true;
}

bool Renderer::install_headless_output_ring(const void* const* pixel_buffers,
                                            size_t pixel_buffer_count,
                                            void* displayed_pixel_buffer,
                                            void* protected_pixel_buffer,
                                            int width,
                                            int height,
                                            int max_track_slots) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_ || !presentation_backend_) {
        return false;
    }
    const auto validation =
        validate_renderer_dimensions(width, height, "headless output dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid headless output ring: {}",
                     validation.message);
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
        if (!presentation_backend_->update_headless_output_ring(pixel_buffers,
                                                                pixel_buffer_count,
                                                                displayed_pixel_buffer,
                                                                protected_pixel_buffer,
                                                                width,
                                                                height,
                                                                max_track_slots)) {
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (target_width_ == width && target_height_ == height) {
            return true;
        }
        const auto layout_tracks = snapshot_layout_track_geometry(tracks_);
        adjust_layout_view_offset_for_resize(
            layout_, target_width_, target_height_, width, height, layout_tracks);
        target_width_ = width;
        target_height_ = height;
        preview_drawn_ = false;
    }
    return true;
}

void Renderer::mark_headless_output_displayed(void* pixel_buffer) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_ || !presentation_backend_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    presentation_backend_->mark_headless_output_displayed(pixel_buffer);
}

void Renderer::protect_headless_output(void* pixel_buffer) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_ || !presentation_backend_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    presentation_backend_->protect_headless_output(pixel_buffer);
}

void Renderer::release_headless_output(void* pixel_buffer) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_ || !presentation_backend_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    presentation_backend_->release_headless_output(pixel_buffer);
}

void Renderer::clear_headless_output() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!headless_ || !presentation_backend_) {
        return;
    }
    std::lock_guard<std::recursive_mutex> ctx_lock(device_mutex_);
    presentation_backend_->clear_headless_output();
}

} // namespace vr
