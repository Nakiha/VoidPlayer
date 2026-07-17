#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/color/color_reference.h"
#include "renderer/frame/frame_storage.h"
#include "renderer/track/track_info.h"
#include "windows/presentation/windows_display_resolver.h"
#include "windows/presentation/windows_presentation_policy.h"

using Catch::Approx;

TEST_CASE("Windows Auto selects scRGB only for HDR on active matching output",
          "[windows_presentation][windows_hdr]") {
  vr::WindowsDisplayProbeResult display;
  display.output_resolved = true;
  display.color_metadata_available = true;
  display.hdr_active = true;
  display.matches_presentation_adapter = true;

  const auto hdr =
      vr::resolve_windows_presentation_policy("auto", true, display);
  REQUIRE(hdr.mode == "native-compositor-scrgb");
  REQUIRE(hdr.reason == "auto-hdr-track");
  REQUIRE(hdr.output_target == vr::ColorOutputTarget::kWindowsLinearScRGB);
  REQUIRE(hdr.hdr_output_requested);

  const auto sdr =
      vr::resolve_windows_presentation_policy("auto", false, display);
  REQUIRE(sdr.mode == "native-compositor-sdr");
  REQUIRE(sdr.reason == "auto-sdr-only");
  REQUIRE_FALSE(sdr.hdr_output_requested);
}

TEST_CASE("Windows Auto HDR has explicit display and adapter fallbacks",
          "[windows_presentation][windows_hdr]") {
  vr::WindowsDisplayProbeResult display;
  display.output_resolved = true;
  display.color_metadata_available = true;
  display.hdr_active = false;
  display.matches_presentation_adapter = true;
  const auto inactive =
      vr::resolve_windows_presentation_policy("auto", true, display);
  REQUIRE(inactive.mode == "native-compositor-sdr");
  REQUIRE(inactive.reason == "auto-hdr-display-unavailable");

  display.hdr_active = true;
  display.matches_presentation_adapter = false;
  const auto mismatch =
      vr::resolve_windows_presentation_policy("auto", true, display);
  REQUIRE(mismatch.mode == "native-compositor-sdr");
  REQUIRE(mismatch.reason == "auto-hdr-cross-adapter-unsupported");
  REQUIRE(mismatch.fallback_reason == "cross-adapter-output-unsupported");
}

TEST_CASE("Windows presentation modes remain native-compositor only",
          "[windows_presentation][windows_hdr]") {
  vr::WindowsDisplayProbeResult display;
  const auto forced_sdr = vr::resolve_windows_presentation_policy(
      " native-compositor-sdr ", true, display);
  REQUIRE(forced_sdr.mode == "native-compositor-sdr");
  REQUIRE_FALSE(forced_sdr.auto_enabled);

  const auto forced_hdr =
      vr::resolve_windows_presentation_policy(" HDR ", true, display);
  REQUIRE(forced_hdr.mode == "native-compositor-scrgb");
  REQUIRE(forced_hdr.output_target ==
          vr::ColorOutputTarget::kWindowsLinearScRGB);

  const auto texture = vr::resolve_windows_presentation_policy(
      "flutter-texture-sdr", true, display);
  REQUIRE_FALSE(texture.supported);
  REQUIRE(texture.mode == "unsupported");
}

TEST_CASE("Windows HDR track detection uses decoder transfer metadata",
          "[windows_presentation][windows_hdr]") {
  std::vector<vr::TrackInfo> tracks(2);
  tracks[0].color.transfer = vr::VIDEO_COLOR_TRANSFER_SDR;
  tracks[1].color.transfer = vr::VIDEO_COLOR_TRANSFER_UNKNOWN;
  REQUIRE_FALSE(vr::windows_tracks_have_hdr_transfer(tracks));
  tracks[1].color.transfer = vr::VIDEO_COLOR_TRANSFER_HLG;
  REQUIRE(vr::windows_tracks_have_hdr_transfer(tracks));
  tracks[1].color.transfer = vr::VIDEO_COLOR_TRANSFER_PQ;
  REQUIRE(vr::windows_tracks_have_hdr_transfer(tracks));
}

TEST_CASE("Windows SDR white level and scRGB scale share 80 nit units",
          "[windows_presentation][windows_hdr][color]") {
  REQUIRE(vr::windows_sdr_white_level_milli_nits(1000) == 80000);
  REQUIRE(vr::windows_sdr_white_level_milli_nits(2538) == 203040);

  const auto mapped = vr::color_reference_map_to_windows_scrgb(
      {1.0, 0.5, 0.0}, vr::VIDEO_COLOR_TRANSFER_SDR,
      vr::VIDEO_COLOR_PRIMARIES_BT709, 203.0);
  REQUIRE(mapped.r == Approx(203.0 / 80.0));
  REQUIRE(mapped.g ==
          Approx(vr::color_reference_srgb_to_linear(0.5) * 203.0 / 80.0));
  REQUIRE(mapped.r > 1.0);
}
