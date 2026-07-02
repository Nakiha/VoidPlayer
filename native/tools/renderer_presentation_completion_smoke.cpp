#include "renderer/render/renderer_presentation_completion.h"

#include <cstdlib>
#include <iostream>

int main() {
    vr::RendererPresentationCompletionInput input;
    input.attempted_draw = true;
    input.drew = false;
    input.frame_callback_available = true;
    input.frame_failure_callback_available = true;
    input.frame_failure_error =
        "renderer-owned wgpu-metal stale async draw dropped";

    const auto decision = vr::plan_presentation_completion(input);
    if (!decision.callback_available ||
        decision.callback_published ||
        decision.transient_backpressure ||
        decision.notify_frame_failure) {
        std::cerr << "stale async source drop was not treated as nonfatal\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
