#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/color/color_reference.h"
#include "renderer/frame/frame_storage.h"
#include "renderer/track/track_info.h"
#include "windows/presentation/windows_presentation_policy.h"

using Catch::Approx;

TEST_CASE("Windows presentation policy defaults to native compositor Auto",
          "[windows_presentation]") {
    const auto empty = vr::resolve_windows_presentation_policy("");

    REQUIRE(empty.request == "auto");
    REQUIRE(empty.mode == "native-compositor-sdr");
    REQUIRE(empty.reason == "auto-sdr-only");
    REQUIRE(empty.auto_enabled);
    REQUIRE(empty.fp16_scrgb_requested);
    REQUIRE(empty.native_compositor_requested);
}

TEST_CASE("Windows presentation policy maps explicit SDR to native compositor",
          "[windows_presentation]") {
    const auto explicit_sdr = vr::resolve_windows_presentation_policy(" SDR ");

    REQUIRE(explicit_sdr.request == "sdr");
    REQUIRE(explicit_sdr.mode == "native-compositor-sdr");
    REQUIRE(explicit_sdr.reason == "forced-native-compositor-sdr");
    REQUIRE_FALSE(explicit_sdr.auto_enabled);
    REQUIRE(explicit_sdr.fp16_scrgb_requested);
    REQUIRE(explicit_sdr.native_compositor_requested);
    REQUIRE(explicit_sdr.supported);
    REQUIRE(explicit_sdr.fallback_reason == "none");
}

TEST_CASE("Windows presentation policy fail-closes legacy texture modes",
          "[windows_presentation]") {
    const auto fp16 =
        vr::resolve_windows_presentation_policy(" FP16-scRGB ");
    REQUIRE(fp16.request == "fp16-scrgb");
    REQUIRE(fp16.mode == "unsupported");
    REQUIRE(fp16.reason == "unsupported-windows-presentation-mode");
    REQUIRE_FALSE(fp16.supported);
    REQUIRE_FALSE(fp16.fp16_scrgb_requested);
    REQUIRE_FALSE(fp16.native_compositor_requested);

    const auto flutter =
        vr::resolve_windows_presentation_policy("flutter-texture-sdr");
    REQUIRE(flutter.request == "flutter-texture-sdr");
    REQUIRE(flutter.mode == "unsupported");
    REQUIRE_FALSE(flutter.supported);

    const auto unknown =
        vr::resolve_windows_presentation_policy("future-hdr");
    REQUIRE(unknown.request == "future-hdr");
    REQUIRE(unknown.mode == "unsupported");
    REQUIRE(unknown.reason == "unsupported-windows-presentation-mode");
    REQUIRE(unknown.fallback_reason ==
            "unsupported-windows-presentation-mode");
    REQUIRE_FALSE(unknown.supported);
}

TEST_CASE("Windows Auto promotes HDR tracks only on matching HDR output",
          "[windows_presentation][windows_display]") {
    vr::WindowsDisplayProbeResult display;
    display.output_resolved = true;
    display.color_metadata_available = true;
    display.hdr_active = true;
    display.matches_presentation_adapter = true;

    const auto promoted =
        vr::resolve_windows_presentation_policy("auto", true, display);
    REQUIRE(promoted.mode == "native-compositor-scrgb");
    REQUIRE(promoted.reason == "auto-hdr-track");
    REQUIRE(promoted.hdr_output_requested);

    display.hdr_active = false;
    const auto unavailable =
        vr::resolve_windows_presentation_policy("auto", true, display);
    REQUIRE(unavailable.mode == "native-compositor-sdr");
    REQUIRE(unavailable.reason == "auto-hdr-display-unavailable");
    REQUIRE_FALSE(unavailable.hdr_output_requested);

    display.hdr_active = true;
    display.matches_presentation_adapter = false;
    const auto mismatch =
        vr::resolve_windows_presentation_policy("auto", true, display);
    REQUIRE(mismatch.mode == "native-compositor-scrgb");
    REQUIRE(mismatch.reason == "auto-hdr-cross-adapter");
    REQUIRE(mismatch.hdr_output_requested);
    REQUIRE(mismatch.cross_adapter_required);
    REQUIRE(mismatch.cross_adapter_migration_requested);

    display.output_resolved = false;
    const auto transient =
        vr::resolve_windows_presentation_policy("auto", true, display);
    REQUIRE(transient.mode == "native-compositor-sdr");
    REQUIRE(transient.reason == "auto-hdr-display-unavailable");

    display.output_resolved = true;
    display.color_metadata_available = false;
    const auto no_metadata =
        vr::resolve_windows_presentation_policy("auto", true, display);
    REQUIRE(no_metadata.mode == "native-compositor-sdr");
    REQUIRE(no_metadata.reason == "auto-hdr-display-unavailable");
}

TEST_CASE("Windows HDR track detection ignores unknown transfer",
          "[windows_presentation][color]") {
    std::vector<vr::TrackInfo> tracks(2);
    tracks[0].color.transfer = vr::VIDEO_COLOR_TRANSFER_UNKNOWN;
    tracks[1].color.transfer = vr::VIDEO_COLOR_TRANSFER_SDR;
    REQUIRE_FALSE(vr::windows_tracks_have_hdr_transfer(tracks));

    tracks[1].color.transfer = vr::VIDEO_COLOR_TRANSFER_PQ;
    REQUIRE(vr::windows_tracks_have_hdr_transfer(tracks));
    tracks[1].color.transfer = vr::VIDEO_COLOR_TRANSFER_HLG;
    REQUIRE(vr::windows_tracks_have_hdr_transfer(tracks));
}

TEST_CASE("Windows presentation policy accepts native compositor scRGB opt-in",
          "[windows_presentation][windows_dcomp]") {
    const auto policy = vr::resolve_windows_presentation_policy(
        " native-compositor-scrgb ");
    REQUIRE(policy.request == "native-compositor-scrgb");
    REQUIRE(policy.mode == "native-compositor-scrgb");
    REQUIRE(policy.output_target == vr::ColorOutputTarget::kWindowsLinearScRGB);
    REQUIRE(policy.fp16_scrgb_requested);
    REQUIRE(policy.native_compositor_requested);
    REQUIRE(policy.hdr_output_requested);
    REQUIRE(policy.fallback_reason == "none");
}

TEST_CASE("Windows forced scRGB records cross adapter migration need",
          "[windows_presentation][windows_dcomp]") {
    vr::WindowsDisplayProbeResult display;
    display.output_resolved = true;
    display.matches_presentation_adapter = false;

    const auto policy = vr::resolve_windows_presentation_policy(
        "native-compositor-scrgb", true, display);

    REQUIRE(policy.mode == "native-compositor-scrgb");
    REQUIRE(policy.cross_adapter_required);
    REQUIRE(policy.cross_adapter_migration_requested);
}

TEST_CASE("Windows presentation policy supports forced native SDR",
          "[windows_presentation][windows_dcomp]") {
    vr::WindowsDisplayProbeResult display;
    display.output_resolved = true;
    display.color_metadata_available = true;
    display.hdr_active = true;
    display.matches_presentation_adapter = true;

    const auto policy = vr::resolve_windows_presentation_policy(
        " native-compositor-sdr ", true, display);
    REQUIRE(policy.request == "native-compositor-sdr");
    REQUIRE(policy.mode == "native-compositor-sdr");
    REQUIRE(policy.reason == "forced-native-compositor-sdr");
    REQUIRE_FALSE(policy.auto_enabled);
    REQUIRE(policy.has_hdr_track);
    REQUIRE(policy.native_compositor_requested);
    REQUIRE_FALSE(policy.hdr_output_requested);
}

TEST_CASE("Windows scRGB maps SDR reference white without clipping",
          "[windows_presentation][color]") {
    const vr::ColorReferenceRgb encoded{1.0, 0.5, 0.0};
    const auto nominal = vr::color_reference_map_to_windows_scrgb(
        encoded,
        vr::VIDEO_COLOR_TRANSFER_SDR,
        vr::VIDEO_COLOR_PRIMARIES_BT709,
        80.0);
    const auto bright = vr::color_reference_map_to_windows_scrgb(
        encoded,
        vr::VIDEO_COLOR_TRANSFER_SDR,
        vr::VIDEO_COLOR_PRIMARIES_BT709,
        203.0);

    REQUIRE(nominal.r == Approx(1.0));
    REQUIRE(nominal.g == Approx(vr::color_reference_srgb_to_linear(0.5)));
    REQUIRE(bright.r == Approx(203.0 / 80.0));
    REQUIRE(bright.r > 1.0);
}

TEST_CASE("Windows scRGB maps PQ absolute luminance to 80 nit units",
          "[windows_presentation][color]") {
    const double pq_80 = 0.4858567654;
    const double pq_1000 = 0.7518270962;
    const auto reference_white = vr::color_reference_map_to_windows_scrgb(
        {pq_80, pq_80, pq_80},
        vr::VIDEO_COLOR_TRANSFER_PQ,
        vr::VIDEO_COLOR_PRIMARIES_BT709,
        80.0);
    const auto highlight = vr::color_reference_map_to_windows_scrgb(
        {pq_1000, pq_1000, pq_1000},
        vr::VIDEO_COLOR_TRANSFER_PQ,
        vr::VIDEO_COLOR_PRIMARIES_BT709,
        80.0);

    REQUIRE(reference_white.r == Approx(1.0).margin(0.002));
    REQUIRE(highlight.r == Approx(12.5).margin(0.02));
    REQUIRE(highlight.r > 1.0);
}

TEST_CASE("Windows scRGB preserves HLG headroom and BT2020 conversion",
          "[windows_presentation][color]") {
    const auto hlg = vr::color_reference_map_to_windows_scrgb(
        {1.0, 1.0, 1.0},
        vr::VIDEO_COLOR_TRANSFER_HLG,
        vr::VIDEO_COLOR_PRIMARIES_BT2020,
        80.0);
    const auto red = vr::color_reference_map_to_windows_scrgb(
        {1.0, 0.0, 0.0},
        vr::VIDEO_COLOR_TRANSFER_SDR,
        vr::VIDEO_COLOR_PRIMARIES_BT2020,
        80.0);

    REQUIRE(hlg.r == Approx(4.0 * 203.0 / 80.0).margin(0.01));
    REQUIRE(hlg.g == Approx(hlg.r).margin(0.01));
    REQUIRE(red.r == Approx(vr::kBT2020ToBT709RFromR));
    REQUIRE(red.g == Approx(vr::kBT2020ToBT709GFromR));
    REQUIRE(red.b == Approx(vr::kBT2020ToBT709BFromR));
    REQUIRE(red.g < 0.0);
    REQUIRE(red.b < 0.0);
}
