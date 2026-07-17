#include "renderer/renderer_internal.h"
#include "renderer/render/renderer_draw_snapshot_builder.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

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

bool first_frame_info_from_decision(const PresentDecision& decision,
                                    uint64_t layout_revision,
                                    PresentationBackendFrameInfo* out) {
    if (!out) {
        return false;
    }
    for (const auto& frame : decision.frames) {
        if (!frame.has_value()) {
            continue;
        }
        const auto& value = *frame;
        out->width = value.width;
        out->height = value.height;
        out->pts_us = value.pts_us;
        out->dts_us = value.dts_us;
        out->duration_us = value.duration_us;
        out->analysis_frame_index = value.analysis_frame_index;
        out->frame_identity_mode =
            static_cast<int32_t>(value.frame_identity_mode);
        out->source_packet_index = value.source_packet_index;
        out->source_packet_size = value.source_packet_size;
        out->source_packet_pos = value.source_packet_pos;
        out->source_packet_pts = value.source_packet_pts;
        out->source_packet_dts = value.source_packet_dts;
        out->color_range = value.color.range != VIDEO_COLOR_RANGE_UNKNOWN
            ? value.color.range
            : VIDEO_COLOR_RANGE_LIMITED;
        out->color_matrix = value.color.matrix != VIDEO_COLOR_MATRIX_UNKNOWN
            ? value.color.matrix
            : default_presentation_color_matrix_for_size(value.width, value.height);
        out->color_transfer = value.color.transfer != VIDEO_COLOR_TRANSFER_UNKNOWN
            ? value.color.transfer
            : VIDEO_COLOR_TRANSFER_SDR;
        out->color_primaries =
            value.color.primaries != VIDEO_COLOR_PRIMARIES_UNKNOWN
            ? value.color.primaries
            : default_presentation_color_primaries_for_matrix(out->color_matrix);
        out->target_pixel_buffer_address = 0;
        out->layout_revision = layout_revision;
        return true;
    }
    return false;
}

}  // namespace

void Renderer::Impl::set_frame_callback(RendererFrameCallback cb) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (presentation_.set_renderer_managed_offscreen_frame_callback(cb)) {
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

bool Renderer::Impl::update_presentation_sdr_white_level(double nits) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    return presentation_.update_sdr_white_level(nits);
#else
    (void)nits;
    return false;
#endif
}

bool Renderer::Impl::prewarm_presentation_target(int width, int height) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.offscreen() || !presentation_.has_backend()) return false;
    const auto validation =
        validate_renderer_dimensions(width, height, "prewarm dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid prewarm: {}",
                     validation.message);
        return false;
    }
    return presentation_.prewarm_renderer_managed_offscreen_target(width, height);
}

void Renderer::Impl::resize(int width, int height) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.offscreen() || !presentation_.has_backend()) return;
    const auto validation = validate_renderer_dimensions(width, height, "resize dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid resize: {}", validation.message);
        return;
    }
    loop_driver_.request_resize(width, height);
}

bool Renderer::Impl::update_offscreen_target(void* output,
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
        if (!surface_state_.offscreen() || !presentation_.has_backend()) {
            return false;
        }
        const auto validation =
            validate_renderer_dimensions(width, height, "offscreen output dimensions");
        if (!validation.ok) {
            spdlog::warn("[Renderer] ignoring invalid offscreen output: {}",
                         validation.message);
            return false;
        }
        if (!presentation_.update_offscreen_target(
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
                                            "install_offscreen_target",
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

bool Renderer::Impl::install_offscreen_target(void* output,
                                       int width,
                                       int height,
                                       int max_track_slots) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.offscreen() || !presentation_.has_backend()) {
        return false;
    }
    const auto validation =
        validate_renderer_dimensions(width, height, "offscreen output dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid offscreen output: {}",
                     validation.message);
        return false;
    }
    if (!presentation_.update_offscreen_target(
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
        loop_driver_.reset_presentation_scheduler();
    }
    return true;
}

bool Renderer::Impl::install_offscreen_target_ring(const void* const* pixel_buffers,
                                            size_t pixel_buffer_count,
                                            void* displayed_pixel_buffer,
                                            void* protected_pixel_buffer,
                                            int width,
                                            int height,
                                            int max_track_slots) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.offscreen() || !presentation_.has_backend()) {
        return false;
    }
    const auto validation =
        validate_renderer_dimensions(width, height, "offscreen output dimensions");
    if (!validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid offscreen output ring: {}",
                     validation.message);
        return false;
    }
    if (!presentation_.update_offscreen_target_ring(pixel_buffers,
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
        loop_driver_.reset_presentation_scheduler();
    }
    return true;
}

void Renderer::Impl::mark_offscreen_target_displayed(void* pixel_buffer) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.offscreen() || !presentation_.has_backend()) {
        return;
    }
    presentation_.mark_offscreen_target_displayed(pixel_buffer);
}

void Renderer::Impl::protect_offscreen_target(void* pixel_buffer) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.offscreen() || !presentation_.has_backend()) {
        return;
    }
    presentation_.protect_offscreen_target(pixel_buffer);
}

void Renderer::Impl::release_offscreen_target(void* pixel_buffer) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.offscreen() || !presentation_.has_backend()) {
        return;
    }
    presentation_.release_offscreen_target(pixel_buffer);
}

void Renderer::Impl::clear_offscreen_target() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.offscreen() || !presentation_.has_backend()) {
        return;
    }
    presentation_.clear_offscreen_target();
}

bool Renderer::Impl::commit_paused_preview_frame(
    int timeout_ms,
    PresentationBackendFrameInfo* out,
    std::string* error) {
    const auto timeout = std::chrono::milliseconds(std::max(0, timeout_ms));
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool first_attempt = true;
    PresentDecision committed_decision;

    while (first_attempt || std::chrono::steady_clock::now() < deadline) {
        first_attempt = false;
        bool committed = false;
        {
            std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
            if (!initialized_.load(std::memory_order_acquire) ||
                shutting_down_.load(std::memory_order_acquire)) {
                return set_error(error, "renderer is not initialized");
            }
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto preview = track_controller_.paused_refresh_decision(
                present_history_.snapshot(), std::nullopt, true);
            if (preview.has_frame) {
                track_controller_.filter_present_decision(preview.decision);
                if (present_decision_has_frame(preview.decision)) {
                    RendererDrawSnapshotBuilder::update_track_geometry_from_decision(
                        track_controller_, preview.decision);
                    const auto layout_revision = layout_state_.current_revision();
                    present_history_.set(preview.decision);
                    loop_driver_.mark_preview_presented(true);
                    mark_paused_hevc_seek_preview_drawn_locked();
                    if (out) {
                        *out = {};
                        first_frame_info_from_decision(
                            preview.decision, layout_revision, out);
                    }
                    committed_decision = preview.decision;
                    committed = true;
                }
            }
        }
        if (committed) {
            emit_seek_preview_presented_events(committed_decision);
            if (error) {
                error->clear();
            }
            return true;
        }
        if (timeout_ms <= 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return set_error(error, "paused preview frame is not ready");
}

} // namespace vr
