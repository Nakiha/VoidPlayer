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

TEST_CASE("Windows source projection split ignores tracks after comparison pair",
          "[windows_dcomp][windows_source_projection]") {
    vr::WindowsSourceProjection projection;
    projection.enabled = true;
    projection.mode = 1;
    projection.split_pos = 0.35f;
    projection.active_track_count = 4;
    projection.source_order = {0, 1, 2, 3};
    projection.inv_display_size_x = {1.0f, 1.0f, 1.0f, 1.0f};
    projection.inv_display_size_y = {1.0f, 1.0f, 1.0f, 1.0f};

    const std::array<bool, 4> present = {true, true, true, true};
    const auto left =
        vr::project_windows_source_sample(0.10f, 0.25f, projection, present);
    const auto right =
        vr::project_windows_source_sample(0.90f, 0.25f, projection, present);
    REQUIRE(left.present);
    REQUIRE(left.source_slot == 0);
    REQUIRE(right.present);
    REQUIRE(right.source_slot == 1);

    const std::array<bool, 4> only_extra_present = {
        false, false, true, true};
    const auto absent_left = vr::project_windows_source_sample(
        0.10f, 0.25f, projection, only_extra_present);
    const auto absent_right = vr::project_windows_source_sample(
        0.90f, 0.25f, projection, only_extra_present);
    REQUIRE_FALSE(absent_left.present);
    REQUIRE_FALSE(absent_right.present);
}

TEST_CASE("Windows retained source visuals invert pan zoom projection",
          "[windows_dcomp][windows_source_projection]") {
    vr::WindowsSourceProjection projection;
    projection.enabled = true;
    projection.mode = 0;
    projection.active_track_count = 1;
    projection.source_order = {0, 1, 2, 3};
    projection.display_offset_x[0] = 0.25f;
    projection.display_offset_y[0] = 0.125f;
    projection.inv_display_size_x[0] = 2.0f;
    projection.inv_display_size_y[0] = 4.0f;
    projection.view_offset_uv_x[0] = -0.5f;
    projection.view_offset_uv_y[0] = 0.25f;
    const std::array<bool, 4> present = {true, false, false, false};

    const auto rects = vr::project_windows_retained_source_visuals(
        100.0f, 50.0f, 900.0f, 650.0f, projection, present);
    const auto& rect = rects[0];

    REQUIRE(rect.present);
    REQUIRE(rect.source_slot == 0);
    REQUIRE(rect.left == Catch::Approx(100.0f));
    REQUIRE(rect.top == Catch::Approx(162.5f));
    REQUIRE(rect.right == Catch::Approx(500.0f));
    REQUIRE(rect.bottom == Catch::Approx(312.5f));
    REQUIRE(rect.clip_left == Catch::Approx(100.0f));
    REQUIRE(rect.clip_top == Catch::Approx(50.0f));
    REQUIRE(rect.clip_right == Catch::Approx(900.0f));
    REQUIRE(rect.clip_bottom == Catch::Approx(650.0f));
}

TEST_CASE("Windows retained source visuals keep split clips per slot",
          "[windows_dcomp][windows_source_projection]") {
    vr::WindowsSourceProjection projection;
    projection.enabled = true;
    projection.mode = 1;
    projection.split_pos = 0.4f;
    projection.active_track_count = 2;
    projection.source_order = {1, 3, 0, 0};
    projection.inv_display_size_x[1] = 1.0f;
    projection.inv_display_size_y[1] = 1.0f;
    projection.inv_display_size_x[3] = 2.0f;
    projection.inv_display_size_y[3] = 2.0f;
    const std::array<bool, 4> present = {false, true, false, true};

    const auto rects = vr::project_windows_retained_source_visuals(
        0.0f, 0.0f, 1000.0f, 500.0f, projection, present);

    REQUIRE(rects[1].present);
    REQUIRE(rects[1].left == Catch::Approx(0.0f));
    REQUIRE(rects[1].right == Catch::Approx(1000.0f));
    REQUIRE(rects[1].clip_left == Catch::Approx(0.0f));
    REQUIRE(rects[1].clip_right == Catch::Approx(400.0f));
    REQUIRE(rects[3].present);
    REQUIRE(rects[3].left == Catch::Approx(0.0f));
    REQUIRE(rects[3].right == Catch::Approx(500.0f));
    REQUIRE(rects[3].clip_left == Catch::Approx(400.0f));
    REQUIRE(rects[3].clip_right == Catch::Approx(1000.0f));
}

TEST_CASE("Windows retained source visuals split keeps only comparison pair",
          "[windows_dcomp][windows_source_projection]") {
    vr::WindowsSourceProjection projection;
    projection.enabled = true;
    projection.mode = 1;
    projection.split_pos = 0.35f;
    projection.active_track_count = 4;
    projection.source_order = {0, 1, 2, 3};
    projection.inv_display_size_x = {1.0f, 1.0f, 1.0f, 1.0f};
    projection.inv_display_size_y = {1.0f, 1.0f, 1.0f, 1.0f};
    const std::array<bool, 4> present = {true, true, true, true};

    const auto rects = vr::project_windows_retained_source_visuals(
        100.0f, 50.0f, 1100.0f, 650.0f, projection, present);

    REQUIRE(rects[0].present);
    REQUIRE(rects[0].left == Catch::Approx(100.0f));
    REQUIRE(rects[0].right == Catch::Approx(1100.0f));
    REQUIRE(rects[0].clip_left == Catch::Approx(100.0f));
    REQUIRE(rects[0].clip_right == Catch::Approx(450.0f));
    REQUIRE(rects[1].present);
    REQUIRE(rects[1].left == Catch::Approx(100.0f));
    REQUIRE(rects[1].right == Catch::Approx(1100.0f));
    REQUIRE(rects[1].clip_left == Catch::Approx(450.0f));
    REQUIRE(rects[1].clip_right == Catch::Approx(1100.0f));
    REQUIRE_FALSE(rects[2].present);
    REQUIRE_FALSE(rects[3].present);
}
