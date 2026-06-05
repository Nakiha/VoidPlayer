#include "renderer/render/renderer_preview_state.h"

namespace vr {

void RendererPreviewState::reset() {
    drawn_ = false;
    pending_ = false;
}

void RendererPreviewState::request_redraw() {
    drawn_ = false;
}

void RendererPreviewState::mark_presented(bool drawn) {
    drawn_ = drawn;
    pending_ = false;
}

void RendererPreviewState::clear_pending() {
    pending_ = false;
}

bool RendererPreviewState::drawn() const {
    return drawn_;
}

bool RendererPreviewState::pending() const {
    return pending_;
}

bool RendererPreviewState::begin_draw_if_needed() {
    const bool should_draw = !drawn_ && !pending_;
    if (should_draw) {
        pending_ = true;
    }
    return should_draw;
}

void RendererPreviewState::mark_async_pending() {
    pending_ = true;
}

} // namespace vr
