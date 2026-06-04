#include "renderer/renderer_internal.h"

namespace vr {

void Renderer::apply_layout_locked(const LayoutState& state, uint64_t revision) {
    layout_controller_.apply(
        layout_, state, [this](int file_id) { return find_slot_by_file_id(file_id); });
    layout_revision_ = std::max(layout_revision_ + 1, revision);
    preview_drawn_ = false;
}

bool Renderer::should_present_frame_consume_pending_layout() const {
    if (!playing_.load(std::memory_order_acquire) ||
        playback_->clock().is_paused()) {
        return true;
    }
    if (!presentation_backend_ ||
        presentation_backend_->kind() != PresentationBackendKind::Metal) {
        return true;
    }
    return false;
}

void Renderer::note_viewport_compositor_activity() {
    const auto active_until =
        steady_clock_us_now() +
        std::chrono::duration_cast<std::chrono::microseconds>(
            kViewportCompositorActivityGrace)
            .count();
    auto current = viewport_compositor_active_until_us_.load(std::memory_order_relaxed);
    while (active_until > current &&
           !viewport_compositor_active_until_us_.compare_exchange_weak(
               current,
               active_until,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

bool Renderer::should_suppress_playback_present_for_viewport_compositor() const {
    if (!playing_.load(std::memory_order_acquire) ||
        playback_->clock().is_paused() ||
        !presentation_backend_ ||
        presentation_backend_->kind() != PresentationBackendKind::Metal) {
        return false;
    }
    return steady_clock_us_now() <
           viewport_compositor_active_until_us_.load(std::memory_order_relaxed);
}

bool Renderer::consume_pending_layout_locked() {
    std::optional<LayoutState> pending;
    uint64_t pending_revision = 0;
    {
        std::lock_guard<std::mutex> lock(pending_layout_mutex_);
        if (!pending_layout_.has_value()) {
            return false;
        }
        pending = pending_layout_;
        pending_revision = pending_layout_revision_;
        pending_layout_.reset();
        pending_layout_revision_ = 0;
    }
    if (pending_revision <= layout_revision_) {
        return false;
    }
    apply_layout_locked(*pending, pending_revision);
    return true;
}

void Renderer::clear_pending_layout_intent() {
    std::lock_guard<std::mutex> lock(pending_layout_mutex_);
    pending_layout_.reset();
    pending_layout_revision_ = 0;
}

void Renderer::apply_layout(const LayoutState& state) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (auto validation = validate_layout_state(state); !validation.ok) {
        spdlog::warn("[Renderer] ignoring invalid layout: {}", validation.message);
        return;
    }

    const uint64_t intent_revision =
        layout_intent_revision_.fetch_add(1, std::memory_order_relaxed) + 1;
    presentation_backend_metrics_.layout_intent_count.fetch_add(
        1, std::memory_order_relaxed);
    const bool playing_now = playing_.load(std::memory_order_acquire);
    const bool defer_to_playback = playing_now && !playback_->clock().is_paused();

    if (defer_to_playback) {
        {
            std::lock_guard<std::mutex> lock(pending_layout_mutex_);
            pending_layout_ = state;
            pending_layout_revision_ = intent_revision;
        }
        presentation_backend_metrics_.layout_deferred_to_playback_count.fetch_add(
            1, std::memory_order_relaxed);
        if (should_log_viewport_trace_event(false)) {
            spdlog::info(
                "[ViewportTrace] native source=apply_layout layout_rev={} playing={} "
                "defer_to_playback={} mode={} zoom={:.4f} offset=({:.1f},{:.1f}) "
                "split={:.4f} pixel_mode={}",
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
    apply_layout_locked(state, intent_revision);
    if (should_log_viewport_trace_event(false)) {
        spdlog::info(
            "[ViewportTrace] native source=apply_layout layout_rev={} playing={} "
            "defer_to_playback={} mode={} zoom={:.4f} offset=({:.1f},{:.1f}) "
            "split={:.4f} pixel_mode={}",
            layout_revision_,
            playing_now,
            defer_to_playback,
            layout_.mode,
            layout_.zoom_ratio,
            layout_.view_offset[0],
            layout_.view_offset[1],
            layout_.split_pos,
            layout_.pixel_size_mode);
    }
}

void Renderer::set_background_color(float r, float g, float b, float a) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(state_mutex_);
    background_color_[0] = std::clamp(r, 0.0f, 1.0f);
    background_color_[1] = std::clamp(g, 0.0f, 1.0f);
    background_color_[2] = std::clamp(b, 0.0f, 1.0f);
    background_color_[3] = std::clamp(a, 0.0f, 1.0f);
    preview_drawn_ = false;
}

LayoutState Renderer::layout() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    {
        std::lock_guard<std::mutex> pending_lock(pending_layout_mutex_);
        if (pending_layout_.has_value() &&
            pending_layout_revision_ > layout_revision_) {
            return *pending_layout_;
        }
    }
    return layout_controller_.snapshot(layout_);
}

// -- Dynamic track management --

} // namespace vr
