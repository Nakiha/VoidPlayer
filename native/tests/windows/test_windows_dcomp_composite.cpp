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

TEST_CASE("Windows source projection selects stable side by side order",
          "[windows_dcomp][windows_source_projection]") {
    vr::WindowsSourceProjection projection;
    projection.enabled = true;
    projection.mode = 0;
    projection.active_track_count = 4;
    projection.source_order = {2, 0, 3, 1};
    projection.inv_display_size_x = {1.0f, 1.0f, 1.0f, 1.0f};
    projection.inv_display_size_y = {1.0f, 1.0f, 1.0f, 1.0f};
    const std::array<bool, 4> present = {true, true, true, true};

    const auto first =
        vr::project_windows_source_sample(0.10f, 0.25f, projection, present);
    const auto last =
        vr::project_windows_source_sample(0.90f, 0.75f, projection, present);
    REQUIRE(first.present);
    REQUIRE(first.source_slot == 2);
    REQUIRE(first.u == Catch::Approx(0.4f));
    REQUIRE(last.present);
    REQUIRE(last.source_slot == 1);
    REQUIRE(last.u == Catch::Approx(0.6f));
}

TEST_CASE("Windows source projection applies split pan zoom and background",
          "[windows_dcomp][windows_source_projection]") {
    vr::WindowsSourceProjection projection;
    projection.enabled = true;
    projection.mode = 1;
    projection.split_pos = 0.4f;
    projection.active_track_count = 2;
    projection.source_order = {1, 3, 0, 0};
    projection.display_offset_x[1] = 0.1f;
    projection.inv_display_size_x[1] = 2.0f;
    projection.inv_display_size_y[1] = 2.0f;
    projection.view_offset_uv_x[1] = -0.1f;
    projection.inv_display_size_x[3] = 1.0f;
    projection.inv_display_size_y[3] = 1.0f;
    const std::array<bool, 4> present = {false, true, false, true};

    const auto left =
        vr::project_windows_source_sample(0.2f, 0.25f, projection, present);
    REQUIRE(left.present);
    REQUIRE(left.source_slot == 1);
    REQUIRE(left.u == Catch::Approx(0.3f));
    REQUIRE(left.v == Catch::Approx(0.5f));

    projection.view_offset_uv_x[1] = 2.0f;
    const auto outside =
        vr::project_windows_source_sample(0.2f, 0.25f, projection, present);
    REQUIRE_FALSE(outside.present);

    const std::array<bool, 4> missing = {false, true, false, false};
    const auto absent =
        vr::project_windows_source_sample(0.8f, 0.25f, projection, missing);
    REQUIRE_FALSE(absent.present);
}
