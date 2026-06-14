#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/color/color_reference.h"
#include "renderer/frame/frame_storage.h"
#include "windows/presentation/windows_presentation_policy.h"

using Catch::Approx;

TEST_CASE("Windows presentation policy keeps SDR as the default",
          "[windows_presentation]") {
    const auto empty = vr::resolve_windows_presentation_policy("");
    const auto explicit_sdr = vr::resolve_windows_presentation_policy(" SDR ");

    REQUIRE(empty.request == "sdr");
    REQUIRE(empty.mode == "flutter-texture-sdr");
    REQUIRE_FALSE(empty.fp16_scrgb_requested);
    REQUIRE(explicit_sdr.request == "sdr");
    REQUIRE(explicit_sdr.fallback_reason == "none");
}

TEST_CASE("Windows presentation policy accepts only fp16-scrgb opt-in",
          "[windows_presentation]") {
    const auto fp16 =
        vr::resolve_windows_presentation_policy(" FP16-scRGB ");
    REQUIRE(fp16.request == "fp16-scrgb");
    REQUIRE(fp16.mode == "flutter-texture-sdr-fp16-scrgb");
    REQUIRE(fp16.reason == "forced-fp16-scrgb");
    REQUIRE(fp16.output_target ==
            vr::ColorOutputTarget::kWindowsLinearScRGB);
    REQUIRE(fp16.fp16_scrgb_requested);

    const auto unknown =
        vr::resolve_windows_presentation_policy("future-hdr");
    REQUIRE(unknown.request == "future-hdr");
    REQUIRE(unknown.mode == "flutter-texture-sdr");
    REQUIRE(unknown.reason == "unsupported-presentation-request");
    REQUIRE(unknown.fallback_reason ==
            "unsupported-presentation-request");
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
