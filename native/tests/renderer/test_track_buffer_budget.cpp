#include <catch2/catch_test_macros.hpp>

#include "renderer/track/track_buffer_budget.h"

using namespace vr;

namespace {

DemuxStats make_stats(int width, int height) {
    DemuxStats stats;
    stats.width = width;
    stats.height = height;
    return stats;
}

} // namespace

TEST_CASE("TrackBufferBudget: invalid or small tracks use default frame depth",
          "[track_buffer_budget]") {
    NativeResourceBudget budget = default_native_resource_budget();
    budget.high_resolution_track_pixels = 1000;
    budget.default_track_forward_depth = 4;
    budget.default_track_backward_depth = 1;
    budget.high_resolution_hardware_track_forward_depth = 2;

    REQUIRE_FALSE(is_high_resolution_track(make_stats(0, 100), budget));
    REQUIRE_FALSE(is_high_resolution_track(make_stats(10, 10), budget));

    const auto decision = choose_track_buffer_budget(make_stats(10, 10), true, budget);
    REQUIRE(decision.forward_depth == 4);
    REQUIRE(decision.backward_depth == 1);
    REQUIRE(decision.max_cached_frames() == 5);
    REQUIRE_FALSE(decision.high_resolution);
}

TEST_CASE("TrackBufferBudget: high-resolution software tracks keep default depth",
          "[track_buffer_budget]") {
    NativeResourceBudget budget = default_native_resource_budget();
    budget.high_resolution_track_pixels = 1000;
    budget.default_track_forward_depth = 4;
    budget.default_track_backward_depth = 1;
    budget.high_resolution_hardware_track_forward_depth = 2;

    const auto decision = choose_track_buffer_budget(make_stats(40, 30), false, budget);
    REQUIRE(decision.high_resolution);
    REQUIRE(decision.forward_depth == 4);
    REQUIRE(decision.backward_depth == 1);
}

TEST_CASE("TrackBufferBudget: high-resolution hardware tracks use reduced depth",
          "[track_buffer_budget]") {
    NativeResourceBudget budget = default_native_resource_budget();
    budget.high_resolution_track_pixels = 1000;
    budget.default_track_forward_depth = 4;
    budget.default_track_backward_depth = 1;
    budget.high_resolution_hardware_track_forward_depth = 2;

    const auto decision = choose_track_buffer_budget(make_stats(40, 30), true, budget);
    REQUIRE(decision.high_resolution);
    REQUIRE(decision.forward_depth == 2);
    REQUIRE(decision.backward_depth == 1);
    REQUIRE(decision.max_cached_frames() == 3);
}
