#include "renderer/renderer_internal.h"

namespace vr {

RendererPresentCommandContext Renderer::Impl::present_command_context() {
    return RendererPresentCommandContext{
        state_mutex_,
        track_controller_,
        layout_state_,
        surface_state_,
        presentation_,
        render_sink_.get(),
        present_history_,
        loop_driver_,
        presentation_metrics_,
        shutting_down_,
        RendererPresentCommandHooks{
            [this]() {
                return should_present_frame_consume_pending_layout();
            },
            [this]() {
                consume_pending_layout_locked();
            },
            [this]() {
                return should_suppress_playback_present_for_viewport_compositor();
            },
            [this]() {
                return presentation_overlay_hooks();
            },
            [this](const char* operation) {
                enter_terminal_device_lost_locked(operation);
            },
            [this]() {
                return !timeline_.playing() ||
                       timeline_.playback().clock().is_paused();
            },
        },
    };
}

bool Renderer::Impl::request_frame_refresh(const char* reason) {
    {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
        if (!initialized_.load(std::memory_order_acquire) ||
            shutting_down_.load(std::memory_order_acquire)) {
            return false;
        }
    }
    const char* refresh_reason = reason && reason[0] != '\0'
                                     ? reason
                                     : "request_frame_refresh";
    const bool renderer_owned_refresh =
        std::strcmp(refresh_reason, "macos-renderer-owned-refresh") == 0 ||
        std::strcmp(refresh_reason, "request_frame_refresh") == 0;
    const bool viewport_compositor_refresh =
        std::strcmp(refresh_reason, "macos-renderer-owned-refresh") == 0;
    if (viewport_compositor_refresh) {
        note_viewport_compositor_activity();
    }
    bool has_complete_cached_decision = false;
    bool preview_draw_pending = false;
    if (renderer_owned_refresh) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        has_complete_cached_decision =
            track_controller_.has_complete_cached_decision(
                present_history_.snapshot());
        preview_draw_pending = loop_driver_.preview_pending();
    }
    if (renderer_owned_refresh) {
        if (preview_draw_pending) {
            return true;
        }
        auto present_context = present_command_context();
        if (RendererPresentCommandProcessor::redraw_layout(present_context)) {
            return true;
        }
        if (has_complete_cached_decision ||
            (timeline_.playing() &&
             !timeline_.playback().clock().is_paused())) {
            return false;
        }
    } else if (timeline_.playing() &&
               !timeline_.playback().clock().is_paused()) {
        auto present_context = present_command_context();
        return RendererPresentCommandProcessor::redraw_layout(present_context);
    }
    auto present_context = present_command_context();
    const bool drew = RendererPresentCommandProcessor::draw_paused_frame(
        present_context, refresh_reason);
    if (drew && std::strcmp(refresh_reason, "seek_frame_refresh") == 0) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        mark_paused_hevc_seek_preview_drawn_locked();
    }
    return drew;
}

void Renderer::Impl::enter_terminal_device_lost_locked(const char* operation) {
    const auto plan = plan_renderer_device_lost_transition(
        device_state_.load(std::memory_order_acquire));
    if (!plan.apply) {
        return;
    }

    device_state_.store(plan.pre_terminal_state, std::memory_order_release);
    const long reason = presentation_.device_removed_reason();
    if (plan.count_device_lost) {
        presentation_metrics_.note_device_lost();
    }
    spdlog::error(
        "[Renderer] D3D11 device lost during {}; entering terminal renderer state "
        "(reason={:#x})",
        operation,
        static_cast<unsigned long>(reason));

    loop_driver_.set_running(false);
    timeline_.set_playing(false);
    if (plan.pause_playback) {
        timeline_.playback().pause();
    }
    if (plan.pause_decode) {
        set_decode_paused_for_all_tracks(true);
    }
    if (plan.clear_initialized) {
        initialized_ = false;
    }
    device_state_.store(plan.final_state, std::memory_order_release);
}

void Renderer::Impl::enter_terminal_render_loop_error_locked(const char* reason) {
    const auto plan = plan_renderer_runtime_error_transition(
        device_state_.load(std::memory_order_acquire));
    if (!plan.apply) {
        return;
    }
    device_state_.store(plan.pre_terminal_state, std::memory_order_release);
    spdlog::error(
        "[Renderer] Render loop entered terminal runtime state after {}",
        reason);
    loop_driver_.set_running(false);
    timeline_.set_playing(false);
    if (plan.clear_initialized) {
        initialized_ = false;
    }
    if (plan.pause_playback) {
        timeline_.playback().pause();
    }
    if (plan.pause_decode) {
        set_decode_paused_for_all_tracks(true);
    }
    device_state_.store(plan.final_state, std::memory_order_release);
}

bool Renderer::Impl::capture_front_buffer(std::vector<uint8_t>& bgra, int& width, int& height) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.headless()) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    return presentation_.capture_d3d_headless_front_buffer(bgra, width, height);
#else
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.headless()) {
        bgra.clear();
        width = 0;
        height = 0;
        return false;
    }
    return presentation_.capture_backend_front_buffer(bgra, width, height);
#endif
}

bool Renderer::Impl::capture_front_buffer_region(int x,
                                                 int y,
                                                 int width,
                                                 int height,
                                                 std::vector<uint8_t>& bgra,
                                                 int& region_width,
                                                 int& region_height) {
#ifdef _WIN32
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (!surface_state_.headless()) {
        bgra.clear();
        region_width = 0;
        region_height = 0;
        return false;
    }
    return presentation_.capture_d3d_headless_front_buffer_region(
        x, y, width, height, bgra, region_width, region_height);
#else
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    bgra.clear();
    region_width = 0;
    region_height = 0;
    return false;
#endif
}

} // namespace vr
