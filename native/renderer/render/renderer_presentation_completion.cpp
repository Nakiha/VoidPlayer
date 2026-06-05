#include "renderer/render/renderer_presentation_completion.h"

namespace vr {

RendererPresentationCompletionDecision plan_presentation_completion(
    const RendererPresentationCompletionInput& input) {
    RendererPresentationCompletionDecision decision;
    decision.callback_available = input.frame_callback_available;
    decision.callback_frame_info_ptr = input.completed_frame_info;

    if (input.completed_frame_info) {
        decision.callback_frame_info = *input.completed_frame_info;
        decision.callback_frame_info.layout_revision = input.current_layout_revision;
        decision.callback_frame_info_ptr = &decision.callback_frame_info;
    }

    decision.callback_published =
        input.frame_callback_available &&
        !input.stale_layout_after_draw &&
        !input.shutting_down;
    decision.transient_backpressure =
        input.frame_failure_error &&
        is_transient_presentation_backpressure_error(input.frame_failure_error);
    decision.notify_frame_failure =
        input.attempted_draw &&
        !input.drew &&
        !decision.transient_backpressure &&
        input.frame_failure_callback_available &&
        !input.shutting_down;
    decision.frame_failure_error = input.frame_failure_error
        ? input.frame_failure_error
        : "";
    return decision;
}

} // namespace vr
