#include "windows/presentation/windows_high_refresh_metrics.h"
#include "windows/presentation/windows_overlay_layer_state.h"

#include <cassert>
#include <iostream>

namespace {

vr::WindowsOverlayLayerSignature make_signature(uint64_t generation) {
    vr::WindowsOverlayLayerSignature signature;
    signature.primitive_generation = generation;
    signature.track_signature = 0x1234;
    signature.target_class = 1;
    signature.sdr_white_scale_x1000 = 1250;
    signature.source_width = 1921;
    signature.source_height = 1079;
    signature.fill_rect_count = 8;
    signature.outline_rect_count = 16;
    signature.motion_line_count = 4;
    return signature;
}

} // namespace

int main() {
    vr::WindowsOverlayLayerCacheState layer;
    vr::WindowsHighRefreshMetrics metrics;
    metrics.reset(144);

    assert(layer.prepare(make_signature(1), 4096));
    metrics.record_overlay_layer_raster();
    metrics.record_overlay_layer_upload();
    metrics.record_overlay_raster_us(700);
    metrics.record_overlay_upload_us(120);

    for (int i = 0; i < 120; ++i) {
        assert(!layer.prepare(make_signature(1), 4096));
        layer.composite();
        metrics.record_present_interval_us(6900);
        metrics.record_composite_us(1300);
        metrics.record_acquire_wait_us(300);
        metrics.record_source_projection_reuse();
        metrics.record_overlay_layer_reuse();
        metrics.record_overlay_composite_us(450);
    }

    const auto state = layer.snapshot();
    assert(state.active);
    assert(state.raster_count == 1);
    assert(state.upload_count == 1);
    assert(state.reuse_count == 120);
    assert(state.composite_count == 120);

    const auto pass = metrics.snapshot();
    assert(pass.gate_supported);
    assert(vr::evaluate_windows_high_refresh_gate(
               pass, true, true) == "pass");
    assert(pass.overlay_layer_reuse_count > pass.overlay_layer_raster_count);

    vr::WindowsHighRefreshMetrics failing_metrics;
    failing_metrics.reset(144);
    failing_metrics.record_present_interval_us(6900);
    failing_metrics.record_composite_us(1300);
    failing_metrics.record_overlay_layer_raster();
    failing_metrics.record_source_projection_reuse();
    assert(vr::evaluate_windows_high_refresh_gate(
               failing_metrics.snapshot(), true, true) ==
           "fail-overlay-no-reuse");

    std::cout << "windows retained overlay layer smoke passed\n";
    return 0;
}
