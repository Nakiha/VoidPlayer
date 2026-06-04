#include "renderer/renderer_internal.h"

namespace vr {

void Renderer::emit_event(const RendererEvent& event) {
    if (shutting_down_.load(std::memory_order_acquire)) {
        return;
    }

    RendererEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(event_callback_mutex_);
        if (shutting_down_.load(std::memory_order_acquire)) {
            return;
        }
        callback = event_callback_;
    }
    if (callback) {
        if (event.type == RendererEvent::Type::SeekPreviewPresented) {
            spdlog::info("[Renderer] emit seekPreviewPresented request_id={} file_id={} pts={:.3f}s dts={:.3f}s",
                         event.request_id,
                         event.track_file_id,
                         event.pts_us / 1e6,
                         event.dts_us == kNoTimestampUs ? -1.0 : event.dts_us / 1e6);
        } else if (event.type == RendererEvent::Type::TrackError) {
            spdlog::error("[Renderer] emit trackError file_id={} error_code={:#x}",
                          event.track_file_id,
                          static_cast<unsigned>(event.error_code));
        }
        callback(event);
    }
}

void Renderer::clear_event_callback() {
    std::lock_guard<std::mutex> lock(event_callback_mutex_);
    event_callback_ = {};
}

bool Renderer::has_event_callback_for_test() const {
    std::lock_guard<std::mutex> lock(event_callback_mutex_);
    return static_cast<bool>(event_callback_);
}

void Renderer::enter_terminal_render_loop_error_for_test(const char* reason) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    enter_terminal_render_loop_error_locked(reason);
}

void Renderer::emit_seek_preview_presented_events(const PresentDecision& decision) {
    int64_t request_id = -1;
    int64_t target_pts_us = -1;
    std::vector<SeekPreviewPresentedTrackEvent> events;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (pending_seek_event_request_id_ < 0 || pending_seek_event_emitted_) {
            return;
        }
        request_id = pending_seek_event_request_id_;
        target_pts_us = pending_seek_event_target_pts_us_;
        pending_seek_event_emitted_ = true;
        events = collect_seek_preview_presented_track_events(
            tracks_, decision, request_id, target_pts_us);
    }

    for (const auto& track_event : events) {
        RendererEvent event;
        event.type = RendererEvent::Type::SeekPreviewPresented;
        event.request_id = track_event.request_id;
        event.track_file_id = track_event.file_id;
        event.pts_us = track_event.pts_us;
        event.dts_us = track_event.dts_us;
        event.target_pts_us = track_event.target_pts_us;
        emit_event(event);
    }
}

void Renderer::update_track_geometry_from_decision_locked(const PresentDecision& decision) {
    const auto updates = update_layout_track_geometry_from_decision(tracks_, decision);
    for (const auto& update : updates) {
        spdlog::info(
            "[Renderer] track[{}] display geometry changed: {}x{} aspect={:.6f} -> {}x{} aspect={:.6f}",
            update.slot,
            update.old_width,
            update.old_height,
            update.old_aspect,
            update.new_width,
            update.new_height,
            update.new_aspect);
    }
}

} // namespace vr
