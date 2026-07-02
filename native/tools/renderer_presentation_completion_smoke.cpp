#include "renderer/render/renderer_presentation_completion.h"

#include <cstdlib>
#include <iostream>

bool accepts_nonfatal_drop(const char* error) {
    vr::RendererPresentationCompletionInput input;
    input.attempted_draw = true;
    input.drew = false;
    input.frame_callback_available = true;
    input.frame_failure_callback_available = true;
    input.frame_failure_error = error;

    const auto decision = vr::plan_presentation_completion(input);
    return decision.callback_available &&
           !decision.callback_published &&
           !decision.transient_backpressure &&
           !decision.notify_frame_failure;
}

int main() {
    if (!accepts_nonfatal_drop(
            "renderer-owned wgpu-metal stale async draw dropped") ||
        !accepts_nonfatal_drop(
            "renderer-owned wgpu-metal stale output draw dropped")) {
        std::cerr << "stale async drop was not treated as nonfatal\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
