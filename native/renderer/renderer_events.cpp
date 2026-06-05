#include "renderer/renderer_internal.h"

namespace vr {

void Renderer::Impl::emit_event(const RendererEvent& event) {
    event_bus_.emit(event, shutting_down_);
}

void Renderer::Impl::clear_event_callback() {
    event_bus_.clear();
}

bool Renderer::Impl::has_event_callback_for_test() const {
    return event_bus_.has_callback();
}

void Renderer::Impl::enter_terminal_render_loop_error_for_test(const char* reason) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    enter_terminal_render_loop_error_locked(reason);
}

void Renderer::Impl::emit_seek_preview_presented_events(const PresentDecision& decision) {
    int64_t request_id = -1;
    int64_t target_pts_us = -1;
    std::vector<SeekPreviewPresentedTrackEvent> events;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        const auto pending_event =
            timeline_.mark_pending_seek_preview_event_emitted();
        if (!pending_event.has_value()) {
            return;
        }
        request_id = pending_event->request_id;
        target_pts_us = pending_event->target_pts_us;
        events = track_controller_.collect_seek_preview_presented_events(
            decision, request_id, target_pts_us);
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

} // namespace vr
