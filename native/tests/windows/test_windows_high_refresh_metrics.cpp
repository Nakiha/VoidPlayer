#include "windows/presentation/windows_high_refresh_metrics.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Windows high refresh metrics classify low refresh as functional only",
          "[windows_high_refresh]") {
    vr::WindowsHighRefreshMetrics metrics;
    metrics.reset(60);
    metrics.record_present_interval_us(16000);
    metrics.record_composite_us(1000);

    const auto snapshot = metrics.snapshot();
    REQUIRE_FALSE(snapshot.gate_supported);
    REQUIRE(snapshot.display_hz == 60);
    REQUIRE(snapshot.gate_last_result == "functional-only-low-refresh");
}

TEST_CASE("Windows high refresh metrics pass within high refresh budget",
          "[windows_high_refresh]") {
    vr::WindowsHighRefreshMetrics metrics;
    metrics.reset(60);
    metrics.set_display_hz(144);
    for (int i = 0; i < 20; ++i) {
        metrics.record_present_interval_us(6800);
        metrics.record_composite_us(1200);
        metrics.record_draw_us(350);
        metrics.record_present_block_us(850);
        metrics.record_interaction_input_to_present_us(12000);
        metrics.record_overlay_composite_us(700);
        metrics.record_source_projection_reuse();
        metrics.record_overlay_layer_reuse();
    }

    const auto snapshot = metrics.snapshot();
    REQUIRE(snapshot.gate_supported);
    REQUIRE(snapshot.gate_last_result == "pass");
    REQUIRE(snapshot.present_interval_p95_us == 6800);
    REQUIRE(snapshot.composite_p95_us == 1200);
    REQUIRE(snapshot.draw_p95_us == 350);
    REQUIRE(snapshot.present_block_p95_us == 850);
    REQUIRE(snapshot.interaction_input_to_present_p95_us == 12000);
    REQUIRE(snapshot.source_projection_reuse_count == 20);
    REQUIRE(snapshot.overlay_layer_reuse_count == 20);
    REQUIRE(vr::evaluate_windows_high_refresh_gate(
                snapshot, true, true) == "pass");
}

TEST_CASE("Windows high refresh metrics fail on stale projection redraws",
          "[windows_high_refresh]") {
    vr::WindowsHighRefreshMetrics metrics;
    metrics.reset(120);
    metrics.record_present_interval_us(8000);
    metrics.record_composite_us(1000);
    metrics.record_viewport_redraw_during_projection();

    const auto snapshot = metrics.snapshot();
    REQUIRE(snapshot.gate_supported);
    REQUIRE(snapshot.viewport_redraw_during_projection_count == 1);
    REQUIRE(vr::evaluate_windows_high_refresh_gate(
                snapshot, true, false) == "fail-viewport-redraw");
}

TEST_CASE("Windows high refresh metrics fail when overlay rebuilds without reuse",
          "[windows_high_refresh]") {
    vr::WindowsHighRefreshMetrics metrics;
    metrics.reset(144);
    metrics.record_present_interval_us(6900);
    metrics.record_composite_us(1000);
    metrics.record_overlay_layer_raster();
    metrics.record_overlay_layer_upload();
    metrics.record_overlay_raster_us(1800);
    metrics.record_overlay_upload_us(400);

    const auto snapshot = metrics.snapshot();
    REQUIRE(snapshot.gate_supported);
    REQUIRE(snapshot.overlay_layer_raster_count == 1);
    REQUIRE(snapshot.overlay_raster_p95_us == 1800);
    REQUIRE(snapshot.overlay_upload_p95_us == 400);
    REQUIRE(vr::evaluate_windows_high_refresh_gate(
                snapshot, true, true) == "fail-source-cache-no-reuse");
    metrics.record_source_projection_reuse();
    REQUIRE(vr::evaluate_windows_high_refresh_gate(
                metrics.snapshot(), true, true) == "fail-overlay-no-reuse");
}

TEST_CASE("Windows high refresh metrics compute bounded drop rate",
          "[windows_high_refresh]") {
    vr::WindowsHighRefreshMetrics metrics;
    metrics.reset(120);
    for (int i = 0; i < 99; ++i) {
        metrics.record_present_interval_us(8000);
    }
    metrics.record_drop();
    REQUIRE(metrics.snapshot().drop_rate_x1000 == 10);
    metrics.record_drop();
    metrics.record_drop();
    REQUIRE(vr::evaluate_windows_high_refresh_gate(
                metrics.snapshot(), false, false) == "fail-drop-rate");
}

TEST_CASE("Windows high refresh metrics report specific budget failures",
          "[windows_high_refresh]") {
    vr::WindowsHighRefreshMetrics metrics;
    metrics.reset(144);
    metrics.record_source_projection_reuse();
    metrics.record_present_interval_us(12000);
    REQUIRE(vr::evaluate_windows_high_refresh_gate(
                metrics.snapshot(), true, false) ==
            "fail-present-cadence");

    metrics.reset(144);
    metrics.record_source_projection_reuse();
    metrics.record_interaction_input_to_present_us(18000);
    REQUIRE(vr::evaluate_windows_high_refresh_gate(
                metrics.snapshot(), true, false) ==
            "fail-input-latency");
}
