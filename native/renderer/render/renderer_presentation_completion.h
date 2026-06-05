#pragma once

#include "renderer/render/presentation_backend_types.h"

#include <cstdint>

namespace vr {

struct RendererPresentationCompletionInput {
    bool shutting_down = false;
    bool attempted_draw = false;
    bool drew = false;
    bool stale_layout_after_draw = false;
    bool frame_callback_available = false;
    bool frame_failure_callback_available = false;
    const char* frame_failure_error = nullptr;
    uint64_t current_layout_revision = 0;
    const PresentationBackendFrameInfo* completed_frame_info = nullptr;
};

struct RendererPresentationCompletionDecision {
    bool callback_available = false;
    bool callback_published = false;
    bool transient_backpressure = false;
    bool notify_frame_failure = false;
    const char* frame_failure_error = "";
    PresentationBackendFrameInfo callback_frame_info{};
    const PresentationBackendFrameInfo* callback_frame_info_ptr = nullptr;
};

// Lock contract:
// - Pure presentation completion policy; takes only immutable values.
// - Does not take renderer locks, call callbacks, or mutate metrics/layout.
RendererPresentationCompletionDecision plan_presentation_completion(
    const RendererPresentationCompletionInput& input);

} // namespace vr
