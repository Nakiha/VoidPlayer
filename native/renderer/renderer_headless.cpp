#include "renderer/renderer_internal.h"
#include "renderer/overlay/analysis_overlay_primitives.h"
#include "renderer/render/renderer_draw_snapshot_builder.h"

#ifndef VOID_BUILD_ANALYSIS
#define VOID_BUILD_ANALYSIS 0
#endif

namespace vr {
namespace {

bool set_error(std::string* error, const char* message) {
    if (error) {
        *error = message ? message : "";
    }
    return false;
}

LayoutState source_frame_identity_layout() {
    LayoutState layout;
    layout.mode = LAYOUT_SIDE_BY_SIDE;
    layout.split_pos = 0.5f;
    layout.zoom_ratio = 1.0f;
    layout.view_offset[0] = 0.0f;
    layout.view_offset[1] = 0.0f;
    layout.pixel_size_mode = PIXEL_SIZE_FILL_VIEW;
    layout.order[0] = 0;
    layout.order[1] = -1;
    layout.order[2] = -1;
    layout.order[3] = -1;
    return layout;
}

RendererDrawSnapshot make_source_frame_snapshot(
    const PresentDecision& decision,
    size_t source_slot,
    const float background_color[4]) {
    RendererDrawSnapshot snapshot;
    snapshot.decision.should_present = true;
    snapshot.decision.current_pts_us = decision.current_pts_us;
    snapshot.decision.frames[0] = decision.frames[source_slot];
    snapshot.decision.file_ids[0] = decision.file_ids[source_slot];
    snapshot.decision.track_generations[0] =
        decision.track_generations[source_slot];
    snapshot.layout = source_frame_identity_layout();
    if (snapshot.decision.frames[0].has_value()) {
        const auto& frame = *snapshot.decision.frames[0];
        snapshot.track_geometry[0].active = true;
        snapshot.track_geometry[0].width = frame.width;
        snapshot.track_geometry[0].height = frame.height;
        snapshot.track_geometry[0].aspect =
            frame.height > 0
                ? static_cast<float>(frame.width) / static_cast<float>(frame.height)
                : 1.0f;
        snapshot.tracks[0].active = true;
        snapshot.tracks[0].file_id = snapshot.decision.file_ids[0];
        snapshot.tracks[0].generation = snapshot.decision.track_generations[0];
        snapshot.tracks[0].video_width = frame.width;
        snapshot.tracks[0].video_height = frame.height;
        snapshot.tracks[0].video_aspect = snapshot.track_geometry[0].aspect;
        snapshot.target_width = frame.width;
        snapshot.target_height = frame.height;
    }
    if (background_color) {
        for (int i = 0; i < 4; ++i) {
            snapshot.background_color[i] = background_color[i];
        }
    }
    return snapshot;
}

}  // namespace

void Renderer::Impl::set_frame_callback(RendererFrameCallback cb) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (presentation_.set_renderer_managed_headless_frame_callback(cb)) {
        return;
    }
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

bool Renderer::Impl::acquire_shared_fp16_texture(
    SharedFp16TextureSnapshot& snapshot) const {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return presentation_.acquire_d3d_shared_fp16_texture(snapshot);
#else
    (void)snapshot;
    return false;
#endif
}

void Renderer::Impl::release_shared_fp16_texture(
    int buffer_index, uint64_t ring_generation) const {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    presentation_.release_d3d_shared_fp16_texture(
        buffer_index, ring_generation);
#else
    (void)buffer_index;
    (void)ring_generation;
#endif
}

void Renderer::Impl::set_shared_fp16_frame_callback(
    std::function<void()> cb) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    presentation_.set_d3d_shared_fp16_frame_callback(std::move(cb));
#else
    (void)cb;
#endif
}

bool Renderer::Impl::configure_source_cache(
    const std::vector<SourceCacheTrackDescriptor>& descriptors) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return presentation_.configure_d3d_source_cache(descriptors);
#else
    (void)descriptors;
    return false;
#endif
}

bool Renderer::Impl::update_presentation_sdr_white_level(double nits) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return presentation_.update_sdr_white_level(nits);
#else
    (void)nits;
    return false;
#endif
}

void Renderer::Impl::clear_source_cache(const char* reason) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    presentation_.clear_d3d_source_cache(reason);
#else
    (void)reason;
#endif
}

bool Renderer::Impl::acquire_source_cache_bundle(
    SharedSourceCacheBundleSnapshot& snapshot) const {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return presentation_.acquire_d3d_source_cache_bundle(snapshot);
#else
    (void)snapshot;
    return false;
#endif
}

void Renderer::Impl::release_source_cache_bundle(
    int buffer_index, uint64_t ring_generation) const {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    presentation_.release_d3d_source_cache_bundle(
        buffer_index, ring_generation);
#else
    (void)buffer_index;
    (void)ring_generation;
#endif
}

void Renderer::Impl::set_source_cache_frame_callback(
    std::function<void()> cb) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    presentation_.set_d3d_source_cache_frame_callback(std::move(cb));
#else
    (void)cb;
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

bool Renderer::Impl::draw_current_frame_sources(
    PresentationBackend& backend,
    PresentationSourceFrameTarget* targets,
    size_t target_count,
    std::string* error) {
    if (!targets || target_count == 0) {
        return set_error(error, "source frame bake target list is empty");
    }

    PresentDecision decision;
    float background_color[4] = {};
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!surface_state_.headless() || !presentation_.has_backend()) {
            return set_error(error, "renderer is not using a headless presentation backend");
        }
        // Source-frame bake mirrors the last presented frame (via
        // present_history_ below), which is valid whether paused or playing.
        // The macOS compositor drives a per-frame bake during a live viewport
        // interaction so playing-state pan reveals source pixels immediately,
        // exactly like the paused path.
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto decision_result =
            track_controller_.paused_layout_decision(present_history_.snapshot());
        if (!decision_result.has_frame) {
            return set_error(error, "no complete paused frame is available for source bake");
        }
        decision = decision_result.decision;
        track_controller_.filter_present_decision(decision);
        RendererDrawSnapshotBuilder::update_track_geometry_from_decision(
            track_controller_, decision);
        layout_state_.copy_background_color(background_color);
    }

    bool any_drawn = false;
    std::string last_error;
    for (size_t i = 0; i < target_count; ++i) {
        auto& target = targets[i];
        target.drawn = 0;
        target.frame_info = {};
        int slot = target.source_slot;
        if (target.source_file_id >= 0) {
            for (size_t index = 0; index < decision.file_ids.size(); ++index) {
                if (decision.file_ids[index] == target.source_file_id) {
                    slot = static_cast<int>(index);
                    break;
                }
            }
        }
        if (!target.output || slot < 0 ||
            slot >= static_cast<int>(decision.frames.size()) ||
            !decision.frames[static_cast<size_t>(slot)].has_value()) {
            continue;
        }
        const auto& frame = *decision.frames[static_cast<size_t>(slot)];
        if (target.width != frame.width || target.height != frame.height ||
            target.width <= 0 || target.height <= 0) {
            last_error = "source frame bake target dimensions do not match source frame";
            continue;
        }

        auto snapshot = make_source_frame_snapshot(
            decision, static_cast<size_t>(slot), background_color);
        if (!backend.update_headless_output(
                target.output, target.width, target.height, 1)) {
            last_error = backend.last_error();
            if (last_error.empty()) {
                last_error = "source frame bake backend did not accept target";
            }
            continue;
        }
        PresentationBackendDrawHooks hooks;
        hooks.suppress_analysis_overlay = true;
        if (!backend.draw_frame(snapshot, hooks)) {
            last_error = backend.last_error();
            if (last_error.empty()) {
                last_error = "source frame bake draw failed";
            }
            continue;
        }
        PresentationBackendFrameInfo frame_info;
        if (backend.copy_last_frame_info(&frame_info)) {
            target.frame_info = frame_info;
        }
        target.source_file_id = decision.file_ids[static_cast<size_t>(slot)];
        target.drawn = 1;
        any_drawn = true;
    }

    if (!any_drawn) {
        return set_error(
            error,
            last_error.empty() ? "source frame bake produced no frames"
                               : last_error.c_str());
    }
    if (error) {
        error->clear();
    }
    return true;
}

std::shared_ptr<const AnalysisOverlayPrimitivePackage>
Renderer::Impl::current_overlay_primitives(std::string* error) {
#if VOID_BUILD_ANALYSIS
    RendererDrawSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!presentation_.has_backend()) {
            set_error(error, "renderer does not have a presentation backend");
            return {};
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        snapshot = RendererDrawSnapshotBuilder::build(
            track_controller_,
            layout_state_,
            surface_state_,
            present_history_.snapshot());
    }
    if (error) {
        error->clear();
    }
    return build_analysis_overlay_primitive_package(snapshot);
#else
    if (error) {
        error->clear();
    }
    return {};
#endif
}

} // namespace vr
