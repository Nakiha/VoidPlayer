#include "windows/presentation/windows_high_refresh_metrics.h"

#include <cassert>
#include <iostream>

int main() {
    vr::WindowsHighRefreshMetrics metrics;
    metrics.reset(144);
    for (int i = 0; i < 120; ++i) {
        metrics.record_present_interval_us(6900);
        metrics.record_composite_us(1500);
        metrics.record_acquire_wait_us(400);
        metrics.record_interaction_input_to_present_us(12000);
        metrics.record_source_projection_reuse();
        metrics.record_overlay_layer_reuse();
        metrics.record_overlay_composite_us(900);
    }
    const auto pass = metrics.snapshot();
    assert(pass.gate_supported);
    assert(vr::evaluate_windows_high_refresh_gate(
               pass, true, true) == "pass");
    assert(pass.source_projection_reuse_count == 120);
    assert(pass.viewport_redraw_during_projection_count == 0);

    metrics.record_viewport_redraw_during_projection();
    const auto fail = metrics.snapshot();
    assert(vr::evaluate_windows_high_refresh_gate(
               fail, true, true) == "fail-viewport-redraw");

    std::cout << "windows high refresh projection/overlay smoke passed\n";
    return 0;
}
