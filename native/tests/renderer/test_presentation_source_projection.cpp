#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/render/presentation_source_projection.h"

#include <limits>
#include <string>

TEST_CASE("Presentation source projection selects stable side by side order",
          "[presentation_source_projection]") {
    vr::PresentationSourceProjection projection;
    projection.enabled = true;
    projection.mode = 0;
    projection.active_track_count = 4;
    projection.source_order = {2, 0, 3, 1};
    projection.inv_display_size_x = {1.0f, 1.0f, 1.0f, 1.0f};
    projection.inv_display_size_y = {1.0f, 1.0f, 1.0f, 1.0f};
    const std::array<bool, 4> present = {true, true, true, true};

    const auto first =
        vr::project_presentation_source_sample(0.10f, 0.25f, projection, present);
    const auto last =
        vr::project_presentation_source_sample(0.90f, 0.75f, projection, present);

    REQUIRE(first.present);
    REQUIRE(first.source_slot == 2);
    REQUIRE(first.u == Catch::Approx(0.4f));
    REQUIRE(last.present);
    REQUIRE(last.source_slot == 1);
    REQUIRE(last.u == Catch::Approx(0.6f));
}

TEST_CASE("Presentation source projection applies split pan zoom and missing slots",
          "[presentation_source_projection]") {
    vr::PresentationSourceProjection projection;
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
        vr::project_presentation_source_sample(0.2f, 0.25f, projection, present);
    REQUIRE(left.present);
    REQUIRE(left.source_slot == 1);
    REQUIRE(left.u == Catch::Approx(0.3f));
    REQUIRE(left.v == Catch::Approx(0.5f));

    projection.view_offset_uv_x[1] = 2.0f;
    const auto outside =
        vr::project_presentation_source_sample(0.2f, 0.25f, projection, present);
    REQUIRE_FALSE(outside.present);

    const std::array<bool, 4> missing = {false, true, false, false};
    const auto absent =
        vr::project_presentation_source_sample(0.8f, 0.25f, projection, missing);
    REQUIRE_FALSE(absent.present);
}

TEST_CASE("Presentation retained source visuals invert projection",
          "[presentation_source_projection]") {
    vr::PresentationSourceProjection projection;
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

    const auto rects = vr::project_presentation_retained_source_visuals(
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

TEST_CASE("Presentation source projection validation rejects invalid payloads",
          "[presentation_source_projection]") {
    vr::PresentationSourceProjection projection;
    projection.enabled = true;
    projection.mode = 0;
    projection.active_track_count = 2;
    projection.source_order = {0, 1, 2, 3};
    projection.inv_display_size_x = {1.0f, 1.0f, 1.0f, 1.0f};
    projection.inv_display_size_y = {1.0f, 1.0f, 1.0f, 1.0f};
    const std::array<bool, 4> present = {true, true, false, false};
    std::string error;

    REQUIRE(vr::validate_presentation_source_projection(
        projection, present, &error));

    projection.source_order = {0, 0, 2, 3};
    REQUIRE_FALSE(vr::validate_presentation_source_projection(
        projection, present, &error));
    REQUIRE(error == "source projection order contains duplicate slots");

    projection.source_order = {0, 2, 1, 3};
    REQUIRE_FALSE(vr::validate_presentation_source_projection(
        projection, present, &error));
    REQUIRE(error ==
            "source projection order references an unavailable slot");

    projection.source_order = {0, 1, 2, 3};
    projection.split_pos = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(vr::validate_presentation_source_projection(
        projection, present, &error));
    REQUIRE(error == "source projection split position is invalid");
}
