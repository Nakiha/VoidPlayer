#include "windows/presentation/windows_overlay_layer_state.h"

#include <utility>

namespace vr {

bool WindowsOverlayLayerSignature::valid() const {
    return primitive_generation != 0 &&
           source_width != 0 &&
           source_height != 0 &&
           (fill_rect_count != 0 ||
            outline_rect_count != 0 ||
            motion_line_count != 0);
}

bool WindowsOverlayLayerSignature::operator==(
    const WindowsOverlayLayerSignature& other) const {
    return primitive_generation == other.primitive_generation &&
           track_signature == other.track_signature &&
           target_class == other.target_class &&
           sdr_white_scale_x1000 == other.sdr_white_scale_x1000 &&
           source_width == other.source_width &&
           source_height == other.source_height &&
           fill_rect_count == other.fill_rect_count &&
           outline_rect_count == other.outline_rect_count &&
           motion_line_count == other.motion_line_count;
}

void WindowsOverlayLayerCacheState::reset(const std::string& reason) {
    signature_ = {};
    snapshot_ = {};
    snapshot_.fallback_reason = reason.empty() ? "reset" : reason;
    snapshot_.last_error = "none";
}

bool WindowsOverlayLayerCacheState::prepare(
    const WindowsOverlayLayerSignature& signature,
    uint64_t bytes) {
    if (!signature.valid()) {
        miss("invalid-signature");
        return false;
    }
    if (snapshot_.active && signature == signature_) {
        reuse();
        return false;
    }
    signature_ = signature;
    snapshot_.active = true;
    snapshot_.mode = "retained-primitive-buffer";
    snapshot_.generation = signature.primitive_generation;
    snapshot_.pending_generation = signature.primitive_generation;
    snapshot_.committed_generation = signature.primitive_generation;
    snapshot_.texture_count = 1;
    snapshot_.bytes = bytes;
    snapshot_.fallback_reason = "none";
    snapshot_.last_error = "none";
    ++snapshot_.raster_count;
    ++snapshot_.upload_count;
    return true;
}

void WindowsOverlayLayerCacheState::reuse() {
    if (!snapshot_.active) {
        miss("reuse-without-active-layer");
        return;
    }
    ++snapshot_.reuse_count;
}

void WindowsOverlayLayerCacheState::composite() {
    if (!snapshot_.active) {
        miss("composite-without-active-layer");
        return;
    }
    ++snapshot_.composite_count;
}

void WindowsOverlayLayerCacheState::miss(const std::string& reason) {
    ++snapshot_.miss_count;
    snapshot_.last_error = reason.empty() ? "overlay-layer-miss" : reason;
}

void WindowsOverlayLayerCacheState::backpressure(const std::string& reason) {
    ++snapshot_.backpressure_count;
    snapshot_.last_error =
        reason.empty() ? "overlay-layer-backpressure" : reason;
}

void WindowsOverlayLayerCacheState::fail(const std::string& reason) {
    snapshot_.active = false;
    snapshot_.fallback_reason =
        reason.empty() ? "overlay-layer-failed" : reason;
    snapshot_.last_error = snapshot_.fallback_reason;
}

WindowsOverlayLayerStateSnapshot WindowsOverlayLayerCacheState::snapshot()
    const {
    return snapshot_;
}

} // namespace vr
