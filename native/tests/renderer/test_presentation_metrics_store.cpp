#include <catch2/catch_test_macros.hpp>

#include "renderer/metrics/presentation_metrics_store.h"

using namespace vr;

TEST_CASE("Presentation metrics separate renderer work from host callback wait",
          "[renderer][presentation_metrics]") {
    PresentationMetricsStore metrics;

    for (int index = 0; index < 16; ++index) {
        metrics.record_draw_timing(16700, 8300, 8300, 7100);
    }

    const auto snapshot = metrics.snapshot(0, 0);
    REQUIRE(snapshot.draw_count == 16);
    REQUIRE(snapshot.draw_p95_us == 16700);
    REQUIRE(snapshot.draw_work_p95_us == 1300);
    REQUIRE(snapshot.draw_callback_p95_us == 8300);
    REQUIRE(snapshot.draw_blocking_wait_p95_us == 7100);
    REQUIRE(snapshot.draw_backend_p95_us == 8300);
    REQUIRE(snapshot.draw_backend_work_p95_us == 1200);
    REQUIRE(snapshot.draw_total_us ==
            snapshot.draw_work_total_us + snapshot.draw_callback_total_us +
                snapshot.draw_blocking_wait_total_us);
}

TEST_CASE("Presentation metrics saturate callback time at total time",
          "[renderer][presentation_metrics]") {
    PresentationMetricsStore metrics;

    metrics.record_draw_timing(4000, 3000, 6000, 6000);

    const auto snapshot = metrics.snapshot(0, 0);
    REQUIRE(snapshot.draw_p95_us == 4000);
    REQUIRE(snapshot.draw_work_p95_us == 0);
    REQUIRE(snapshot.draw_callback_p95_us == 4000);
    REQUIRE(snapshot.draw_work_total_us == 0);
    REQUIRE(snapshot.draw_callback_total_us == 4000);
    REQUIRE(snapshot.draw_blocking_wait_total_us == 0);
}
