#include "video_renderer/sync/seek_controller.h"
#include <spdlog/spdlog.h>

namespace vr {

SeekController::SeekController()
{}

void SeekController::request_seek(int64_t target_pts_us, SeekType type) {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.target_pts_us = target_pts_us;
    pending_.type = type;
    has_pending_.store(true, std::memory_order_release);
    spdlog::debug("[SeekController] Seek requested: target={:.3f}s, type={}",
                  target_pts_us / 1e6, static_cast<int>(type));
}

bool SeekController::has_pending_seek() const {
    return has_pending_.load(std::memory_order_acquire);
}

std::optional<SeekRequest> SeekController::take_pending() {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    if (!has_pending_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    has_pending_.store(false, std::memory_order_release);
    return pending_;
}

} // namespace vr
