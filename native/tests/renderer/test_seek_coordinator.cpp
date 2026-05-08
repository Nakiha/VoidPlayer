#include <catch2/catch_test_macros.hpp>

#include "video_renderer/seek_coordinator.h"

#include <chrono>
#include <thread>

using namespace vr;

TEST_CASE("SeekCoordinator: only paused exact HEVC hardware seek enters coordinator",
          "[seek][coordinator]") {
    SeekCoordinator coordinator(std::chrono::milliseconds(1));

    REQUIRE_FALSE(coordinator.should_defer_paused_hevc_seek(
        true, true, 1000, SeekType::Exact));
    REQUIRE_FALSE(coordinator.should_defer_paused_hevc_seek(
        false, false, 1000, SeekType::Exact));
    REQUIRE_FALSE(coordinator.should_defer_paused_hevc_seek(
        false, true, 1000, SeekType::Keyframe));
    REQUIRE_FALSE(coordinator.paused_hevc_seek_in_flight());
}

TEST_CASE("SeekCoordinator: serializes paused HEVC exact seeks during settle window",
          "[seek][coordinator]") {
    SeekCoordinator coordinator(std::chrono::milliseconds(1));

    REQUIRE_FALSE(coordinator.should_defer_paused_hevc_seek(
        false, true, 1000, SeekType::Exact));
    REQUIRE(coordinator.paused_hevc_seek_in_flight());

    REQUIRE(coordinator.should_defer_paused_hevc_seek(
        false, true, 2000, SeekType::Exact));
    REQUIRE(coordinator.has_pending_deferred_seek());

    coordinator.mark_paused_hevc_preview_drawn(true);
    REQUIRE_FALSE(coordinator.paused_hevc_seek_in_flight());
    REQUIRE_FALSE(coordinator.take_deferred_paused_hevc_seek(false).has_value());

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto deferred = coordinator.take_deferred_paused_hevc_seek(false);
    REQUIRE(deferred.has_value());
    REQUIRE(deferred->target_pts_us == 2000);
    REQUIRE(deferred->type == SeekType::Exact);
    REQUIRE(coordinator.paused_hevc_seek_in_flight());
}

TEST_CASE("SeekCoordinator: reset clears deferred state", "[seek][coordinator]") {
    SeekCoordinator coordinator(std::chrono::milliseconds(1));

    REQUIRE_FALSE(coordinator.should_defer_paused_hevc_seek(
        false, true, 1000, SeekType::Exact));
    REQUIRE(coordinator.should_defer_paused_hevc_seek(
        false, true, 2000, SeekType::Exact));
    coordinator.reset();

    REQUIRE_FALSE(coordinator.paused_hevc_seek_in_flight());
    REQUIRE_FALSE(coordinator.has_pending_deferred_seek());
}
