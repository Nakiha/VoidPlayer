#include "renderer/events/renderer_event_bus.h"

#include <spdlog/spdlog.h>
#include <utility>

namespace vr {

void RendererEventBus::set_callback(RendererEventCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void RendererEventBus::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = {};
}

bool RendererEventBus::has_callback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<bool>(callback_);
}

void RendererEventBus::emit(const RendererEvent& event,
                            const std::atomic<bool>& shutting_down) const {
    if (shutting_down.load(std::memory_order_acquire)) {
        return;
    }

    RendererEventCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down.load(std::memory_order_acquire)) {
            return;
        }
        callback = callback_;
    }
    if (!callback) {
        return;
    }

    if (event.type == RendererEvent::Type::SeekPreviewPresented) {
        spdlog::debug("[Renderer] emit seekPreviewPresented request_id={} file_id={} pts={:.3f}s dts={:.3f}s",
                     event.request_id,
                     event.track_file_id,
                     event.pts_us / 1e6,
                     event.dts_us == kNoTimestampUs ? -1.0 : event.dts_us / 1e6);
    } else if (event.type == RendererEvent::Type::TrackError) {
        spdlog::error("[Renderer] emit trackError file_id={} error_code={:#x}",
                      event.track_file_id,
                      static_cast<unsigned>(event.error_code));
    } else if (event.type == RendererEvent::Type::PlaybackClock) {
        spdlog::debug("[Renderer] emit playbackClock pts={:.3f}s duration={:.3f}s playing={} speed={:.3f}",
                      event.pts_us / 1e6,
                      event.duration_us / 1e6,
                      event.playing,
                      event.playback_speed);
    } else if (event.type == RendererEvent::Type::PlaybackFrameReady) {
        spdlog::trace("[Renderer] emit playbackFrameReady pts={:.3f}s playing={}",
                      event.pts_us / 1e6,
                      event.playing);
    }
    callback(event);
}

} // namespace vr
