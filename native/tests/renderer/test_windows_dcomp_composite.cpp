#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "windows/presentation/windows_dcomp_composite.h"

TEST_CASE("Windows DComp composite preserves transparent scRGB highlights",
          "[windows_dcomp][windows_presentation]") {
    const vr::WindowsDcompCompositeSample video = {
        12.5f, -0.25f, 0.5f, 1.0f};
    const vr::WindowsDcompCompositeSample transparent = {};

    const auto result =
        vr::composite_windows_dcomp_pixel(video, transparent, 203.0f / 80.0f);

    REQUIRE(result.r == Catch::Approx(12.5f));
    REQUIRE(result.g == Catch::Approx(-0.25f));
    REQUIRE(result.b == Catch::Approx(0.5f));
    REQUIRE(result.a == Catch::Approx(1.0f));
}

TEST_CASE("Windows DComp composite decodes premultiplied Flutter sRGB",
          "[windows_dcomp][windows_presentation]") {
    const vr::WindowsDcompCompositeSample video = {
        4.0f, 0.25f, 0.5f, 1.0f};
    const vr::WindowsDcompCompositeSample flutter = {
        0.5f, 0.0f, 0.0f, 0.5f};

    const auto result =
        vr::composite_windows_dcomp_pixel(video, flutter, 1.0f);

    REQUIRE(result.r == Catch::Approx(2.5f).margin(0.0001f));
    REQUIRE(result.g == Catch::Approx(0.125f).margin(0.0001f));
    REQUIRE(result.b == Catch::Approx(0.25f).margin(0.0001f));
    REQUIRE(result.a == Catch::Approx(1.0f));
}

TEST_CASE("Windows DComp composite applies SDR reference white scale",
          "[windows_dcomp][windows_presentation]") {
    const vr::WindowsDcompCompositeSample video = {
        12.5f, 12.5f, 12.5f, 1.0f};
    const vr::WindowsDcompCompositeSample opaque_white = {
        1.0f, 1.0f, 1.0f, 1.0f};

    const auto result = vr::composite_windows_dcomp_pixel(
        video, opaque_white, 203.0f / 80.0f);

    REQUIRE(result.r == Catch::Approx(203.0f / 80.0f));
    REQUIRE(result.g == Catch::Approx(203.0f / 80.0f));
    REQUIRE(result.b == Catch::Approx(203.0f / 80.0f));
    REQUIRE(result.a == Catch::Approx(1.0f));
}
