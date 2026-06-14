#include <catch2/catch_test_macros.hpp>

#include "windows/presentation/windows_display_resolver.h"

#include <vector>

using namespace vr;

namespace {

WindowsDisplayOutputCandidate output(
    int64_t left,
    int64_t top,
    int64_t right,
    int64_t bottom,
    uint64_t monitor_id,
    bool attached = true) {
    WindowsDisplayOutputCandidate candidate;
    candidate.desktop_rect = {left, top, right, bottom};
    candidate.monitor_id = monitor_id;
    candidate.attached_to_desktop = attached;
    return candidate;
}

} // namespace

TEST_CASE("Windows display resolver chooses greatest intersection across negative coordinates",
          "[windows_display]") {
    const WindowsDisplayRect window{-400, 100, 800, 900};
    const std::vector<WindowsDisplayOutputCandidate> candidates{
        output(-1920, 0, 0, 1080, 1),
        output(0, 0, 1920, 1080, 2),
    };

    const auto selected =
        select_windows_display_output(window, 1, candidates);

    REQUIRE(selected.resolved);
    REQUIRE(selected.candidate_index == 1);
    REQUIRE(selected.intersection_area == 640000);
    REQUIRE(selected.reason ==
            WindowsDisplaySelectionReason::GreatestIntersection);
}

TEST_CASE("Windows display resolver uses nearest monitor when there is no intersection",
          "[windows_display]") {
    const WindowsDisplayRect window{5000, 5000, 5100, 5100};
    const std::vector<WindowsDisplayOutputCandidate> candidates{
        output(0, 0, 1920, 1080, 11),
        output(1920, 0, 3840, 1080, 22),
    };

    const auto selected =
        select_windows_display_output(window, 22, candidates);

    REQUIRE(selected.resolved);
    REQUIRE(selected.candidate_index == 1);
    REQUIRE(selected.intersection_area == 0);
    REQUIRE(selected.reason == WindowsDisplaySelectionReason::NearestMonitor);
}

TEST_CASE("Windows display resolver ignores detached outputs",
          "[windows_display]") {
    const WindowsDisplayRect window{0, 0, 800, 600};
    const std::vector<WindowsDisplayOutputCandidate> candidates{
        output(0, 0, 1920, 1080, 1, false),
        output(1920, 0, 3840, 1080, 2, true),
    };

    const auto selected =
        select_windows_display_output(window, 1, candidates);

    REQUIRE(selected.resolved);
    REQUIRE(selected.candidate_index == 1);
    REQUIRE(selected.reason ==
            WindowsDisplaySelectionReason::FirstAttachedOutput);
}

TEST_CASE("Windows display resolver keeps enumeration order for intersection ties",
          "[windows_display]") {
    const WindowsDisplayRect window{900, 0, 1100, 100};
    const std::vector<WindowsDisplayOutputCandidate> candidates{
        output(0, 0, 1000, 1000, 1),
        output(1000, 0, 2000, 1000, 2),
    };

    const auto selected =
        select_windows_display_output(window, 2, candidates);

    REQUIRE(selected.resolved);
    REQUIRE(selected.candidate_index == 0);
    REQUIRE(selected.intersection_area == 10000);
}

TEST_CASE("Windows display resolver reports unresolved without attached outputs",
          "[windows_display]") {
    const WindowsDisplayRect window{0, 0, 800, 600};
    const std::vector<WindowsDisplayOutputCandidate> candidates{
        output(0, 0, 1920, 1080, 1, false),
    };

    const auto selected =
        select_windows_display_output(window, 1, candidates);

    REQUIRE_FALSE(selected.resolved);
    REQUIRE(selected.reason == WindowsDisplaySelectionReason::None);
}

TEST_CASE("Windows display color classification recognizes PQ and HLG",
          "[windows_display]") {
    REQUIRE(windows_display_color_space_is_hdr(
        DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020));
    REQUIRE(windows_display_color_space_is_hdr(
        DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020));
    REQUIRE_FALSE(windows_display_color_space_is_hdr(
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
    REQUIRE(windows_display_advanced_color_state(
                true, DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) ==
            "hdr-active");
    REQUIRE(windows_display_advanced_color_state(
                true, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) ==
            "sdr-or-advanced-color-unknown");
    REQUIRE(windows_display_advanced_color_state(
                false, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) ==
            "unavailable");
}

TEST_CASE("Windows display color classification preserves unknown enum values",
          "[windows_display]") {
    const auto unknown = static_cast<DXGI_COLOR_SPACE_TYPE>(12345);
    REQUIRE(windows_display_color_space_name(unknown) == "unknown-12345");
    REQUIRE_FALSE(windows_display_color_space_is_hdr(unknown));
}

TEST_CASE("Windows display probe tracker starts at generation one",
          "[windows_display]") {
    WindowsDisplayProbeTracker tracker;
    WindowsDisplayProbeResult probe;
    probe.status = "ok";
    probe.output_resolved = true;
    probe.device_name = "DISPLAY1";

    const auto snapshot = tracker.Update(probe);

    REQUIRE(snapshot.changed);
    REQUIRE(snapshot.generation == 1);
    REQUIRE(snapshot.change_count == 0);
    REQUIRE(snapshot.last_change_reason == "initial-probe");
}

TEST_CASE("Windows display probe tracker ignores intersection-only movement",
          "[windows_display]") {
    WindowsDisplayProbeTracker tracker;
    WindowsDisplayProbeResult probe;
    probe.status = "ok";
    probe.output_resolved = true;
    probe.device_name = "DISPLAY1";
    probe.intersection_area = 100;
    tracker.Update(probe);

    probe.intersection_area = 200;
    probe.selection_reason = "greatest-intersection";
    const auto snapshot = tracker.Update(probe);

    REQUIRE_FALSE(snapshot.changed);
    REQUIRE(snapshot.generation == 1);
    REQUIRE(snapshot.change_count == 0);
    REQUIRE(snapshot.last_change_reason == "initial-probe");
}

TEST_CASE("Windows display probe tracker records output color and geometry changes",
          "[windows_display]") {
    WindowsDisplayProbeTracker tracker;
    WindowsDisplayProbeResult probe;
    probe.status = "ok";
    probe.output_resolved = true;
    probe.device_name = "DISPLAY1";
    probe.color_metadata_available = true;
    probe.color_space = "rgb-full-g22-p709";
    probe.advanced_color_state = "sdr-or-advanced-color-unknown";
    probe.desktop_width = 1920;
    probe.desktop_height = 1080;
    tracker.Update(probe);

    probe.color_space = "rgb-full-pq-p2020";
    probe.advanced_color_state = "hdr-active";
    probe.hdr_active = true;
    auto snapshot = tracker.Update(probe);
    REQUIRE(snapshot.changed);
    REQUIRE(snapshot.generation == 2);
    REQUIRE(snapshot.change_count == 1);
    REQUIRE(snapshot.last_change_reason == "color-state-changed");

    probe.desktop_width = 3840;
    snapshot = tracker.Update(probe);
    REQUIRE(snapshot.changed);
    REQUIRE(snapshot.generation == 3);
    REQUIRE(snapshot.change_count == 2);
    REQUIRE(snapshot.last_change_reason == "output-geometry-changed");

    probe.device_name = "DISPLAY2";
    snapshot = tracker.Update(probe);
    REQUIRE(snapshot.changed);
    REQUIRE(snapshot.generation == 4);
    REQUIRE(snapshot.change_count == 3);
    REQUIRE(snapshot.last_change_reason == "output-changed");
}

TEST_CASE("Windows display probe tracker records probe status changes",
          "[windows_display]") {
    WindowsDisplayProbeTracker tracker;
    WindowsDisplayProbeResult probe;
    probe.status = "ok";
    probe.output_resolved = true;
    probe.device_name = "DISPLAY1";
    tracker.Update(probe);

    probe.status = "dxgi-factory-unavailable";
    probe.output_resolved = false;
    const auto snapshot = tracker.Update(probe);

    REQUIRE(snapshot.changed);
    REQUIRE(snapshot.generation == 2);
    REQUIRE(snapshot.change_count == 1);
    REQUIRE(snapshot.last_change_reason == "probe-status-changed");
}
