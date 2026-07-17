#include "renderer/renderer_internal.h"

#include <atomic>

namespace vr {

void Renderer::Impl::apply_layout_locked(const LayoutState& state,
                                         uint64_t revision,
                                         bool schedule_preview) {
    layout_state_.apply_current(
        state, revision, [this](int file_id) {
            return track_controller_.find_slot_by_file_id(file_id);
        });
    if (schedule_preview) {
        loop_driver_.force_preview_redraw();
    }
}

bool Renderer::Impl::consume_pending_layout_locked() {
    const bool consumed = layout_state_.consume_pending_if_newer(
        [this](int file_id) {
            return track_controller_.find_slot_by_file_id(file_id);
        });
    if (consumed && !interaction_presentation_active()) {
        loop_driver_.force_preview_redraw();
    }
    return consumed;
}

void Renderer::Impl::clear_pending_layout_intent() {
    layout_state_.clear_pending();
}

void Renderer::Impl::apply_layout(const LayoutState& state) {
    apply_layout_impl(state, true, "apply_layout");
}

void Renderer::Impl::apply_interaction_layout(const LayoutState& state) {
    apply_layout_impl(state, false, "apply_interaction_layout");
}

void Renderer::Impl::apply_layout_impl(const LayoutState& state,
                                       bool schedule_preview,
                                       const char* trace_source) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (auto validation = validate_layout_state(state); !validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid layout: {}", validation.message);
        return;
    }

    const uint64_t intent_revision =
        layout_state_.next_intent_revision();
    static std::atomic<uint64_t> layout_log_count{0};
    const auto log_count =
        layout_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool log_layout = log_count <= 12 || log_count % 60 == 0;
    presentation_metrics_.note_layout_intent();
    const bool playing_now = timeline_.playing();
    const bool defer_to_playback = playing_now && !timeline_.playback().clock().is_paused();

    if (defer_to_playback) {
        layout_state_.set_pending(state, intent_revision);
        presentation_metrics_.note_layout_deferred_to_playback();
        if (log_layout || should_log_viewport_trace_event(false)) {
            spdlog::info(
                "[ViewportTrace] native source={} layout_rev={} playing={} "
                "defer_to_playback={} mode={} zoom={:.4f} offset=({:.1f},{:.1f}) "
                "split={:.4f} pixel_mode={}",
                trace_source,
                intent_revision,
                playing_now,
                defer_to_playback,
                state.mode,
                state.zoom_ratio,
                state.view_offset[0],
                state.view_offset[1],
                state.split_pos,
                state.pixel_size_mode);
        }
        return;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    clear_pending_layout_intent();
    apply_layout_locked(state, intent_revision, schedule_preview);
    if (log_layout || should_log_viewport_trace_event(false)) {
        const auto layout = layout_state_.current_for_draw();
        spdlog::info(
            "[ViewportTrace] native source={} layout_rev={} playing={} "
            "defer_to_playback={} mode={} zoom={:.4f} offset=({:.1f},{:.1f}) "
            "split={:.4f} pixel_mode={}",
            trace_source,
            layout_state_.current_revision(),
            playing_now,
            defer_to_playback,
            layout.mode,
            layout.zoom_ratio,
            layout.view_offset[0],
            layout.view_offset[1],
            layout.split_pos,
            layout.pixel_size_mode);
    }
}

void Renderer::Impl::set_background_color(float r, float g, float b, float a) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    layout_state_.set_background_color(r, g, b, a);
    loop_driver_.force_preview_redraw();
}

LayoutState Renderer::Impl::layout() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return layout_state_.public_snapshot();
}

// -- Dynamic track management --

} // namespace vr
