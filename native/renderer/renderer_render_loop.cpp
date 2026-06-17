#include "renderer/renderer_internal.h"
#include "renderer/overlay/analysis_overlay_primitives.h"
#include "renderer/render/renderer_draw_snapshot_builder.h"

#ifdef _WIN32
#include "windows/d3d11/render_backend.h"
#endif

namespace vr {

void Renderer::Impl::do_resize(int width, int height) {
#ifdef _WIN32
    int old_width = 0;
    int old_height = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (width == surface_state_.width() &&
            height == surface_state_.height()) {
            return;
        }
        old_width = surface_state_.width();
        old_height = surface_state_.height();
    }

    spdlog::info("[Renderer] resize: {}x{} -> {}x{}", old_width, old_height, width, height);

    if (!presentation_.resize_renderer_managed_headless_output(
            width, height, presentation_metrics_)) {
        return;
    }

    RendererDrawSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto resize = surface_state_.resize_if_changed(width, height);
        old_width = resize.old_width;
        old_height = resize.old_height;
        const auto layout_tracks = track_controller_.layout_track_geometry();
        layout_state_.adjust_view_offset_for_resize(
            old_width, old_height, width, height, layout_tracks);
        snapshot = RendererDrawSnapshotBuilder::build(track_controller_,
                                                      layout_state_,
                                                      surface_state_,
                                                      present_history_.snapshot());
    }

    RendererFrameCallback frame_callback;
    bool drew = false;
    {
        std::lock_guard<std::recursive_mutex> ctx_lock(presentation_.device_mutex());
        drew = presentation_.draw_renderer_managed_headless_and_publish(
            snapshot,
            "resize",
            presentation_metrics_,
            presentation_overlay_hooks(),
            [this]() { return shutting_down_.load(std::memory_order_acquire); },
            frame_callback);
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
#else
    (void)width;
    (void)height;
#endif
}

void Renderer::Impl::render_loop() noexcept {
    // On Windows this raises timer resolution from the default ~15.6 ms to
    // 1 ms; on other platforms it is a no-op wrapper.
    ScopedRenderThreadTiming render_thread_timing;
    spdlog::info("[Renderer] Render loop started (timer resolution: platform), tid={}",
                 current_render_thread_id_string());

    RendererRenderLoopCommandContext render_loop_context{
        state_mutex_,
        lifecycle_mutex_,
        loop_driver_,
        timeline_,
        track_controller_,
        presentation_,
        present_history_,
        layout_state_,
        presentation_metrics_,
        render_sink_,
        RendererRenderLoopCommandHooks{
            [this](const char* operation) {
                recover_or_enter_terminal_device_lost_locked(operation);
            },
            [this](std::unique_lock<std::mutex>& state_lock) {
                return apply_deferred_paused_hevc_seek_locked(state_lock);
            },
            [this](std::unique_lock<std::mutex>& state_lock) {
                return apply_loop_range_locked(state_lock);
            },
            [this]() {
                consume_pending_layout_locked();
            },
            [this](int width, int height) {
                do_resize(width, height);
            },
            [this]() {
                return present_command_context();
            },
            [this](bool paused) {
                set_decode_paused_for_all_tracks(paused);
            },
            [this]() {
                mark_paused_hevc_seek_preview_drawn_locked();
            },
            [this](const PresentDecision& decision) {
                emit_seek_preview_presented_events(decision);
            },
            [this](bool force) {
                emit_playback_clock_event(force);
            },
            [this](int64_t end_pts_us) {
                return settle_eof_locked(end_pts_us);
            },
        },
    };

    try {
        RendererRenderLoopCommandProcessor::run_body(render_loop_context);
    } catch (const std::exception& e) {
        spdlog::error("[Renderer] Render loop crashed: {}", e.what());
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_render_loop_error_locked("std::exception");
    } catch (...) {
        spdlog::error("[Renderer] Render loop crashed with an unknown exception");
        std::lock_guard<std::mutex> lock(state_mutex_);
        enter_terminal_render_loop_error_locked("unknown exception");
    }

    loop_driver_.clear_pending_resize();
    spdlog::info("[Renderer] Render loop ended");
}

RendererPresentationOverlayHooks Renderer::Impl::presentation_overlay_hooks() {
    RendererPresentationOverlayHooks hooks;
#ifdef _WIN32
    hooks.draw_overlay = [this](PresentationBackend& backend,
                                const RendererDrawSnapshot& draw_snapshot) {
        if (!analysis_overlay_renderer_ ||
            backend.kind() != PresentationBackendKind::D3D11) {
            return;
        }
        auto* d3d = static_cast<D3D11RenderBackend*>(&backend);
        auto* device = d3d->device();
        auto* resources = d3d->resources();
        if (!device || !resources) {
            return;
        }
        analysis_overlay_renderer_->draw(
            draw_snapshot,
            *device,
            *resources,
            draw_snapshot.target_width,
            draw_snapshot.target_height);
    };
#else
    hooks.draw_overlay = [](PresentationBackend& backend,
                            const RendererDrawSnapshot& draw_snapshot) {
        (void)backend;
        (void)draw_snapshot;
    };
#endif
    hooks.composite_bgra_overlay =
        [this](const RendererDrawSnapshot& draw_snapshot,
               uint8_t* target_bgra,
               int target_width,
               int target_height,
               size_t target_stride_bytes) {
            if (!analysis_overlay_renderer_) {
                return false;
            }
            return analysis_overlay_renderer_->composite_bgra(
                draw_snapshot,
                target_bgra,
                target_width,
                target_height,
                target_stride_bytes);
        };
#if VOID_BUILD_ANALYSIS
    hooks.build_overlay_primitives =
        [](const RendererDrawSnapshot& draw_snapshot) {
            return build_analysis_overlay_primitive_package(draw_snapshot);
        };
#else
    hooks.build_overlay_primitives =
        [](const RendererDrawSnapshot&) {
            return std::shared_ptr<const AnalysisOverlayPrimitivePackage>{};
        };
#endif
    return hooks;
}

} // namespace vr
