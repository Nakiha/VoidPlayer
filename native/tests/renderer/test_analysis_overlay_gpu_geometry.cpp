#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/overlay/analysis_overlay_gpu_geometry.h"

#include <cmath>

namespace {

vr::ShaderConstants base_constants() {
  vr::ShaderConstants constants{};
  constants.mode = 0;
  constants.track_count = 1;
  constants.order[0] = 0;
  constants.canvas_width = 200.0f;
  constants.canvas_height = 100.0f;
  constants.inv_display_size_x[0] = 1.0f;
  constants.inv_display_size_y[0] = 1.0f;
  return constants;
}

vr::AnalysisOverlayPrimitivePackage package_with_fill(int slot = 0) {
  vr::AnalysisOverlayPrimitivePackage package;
  vr::AnalysisOverlayTrackPrimitives track;
  track.slot = slot;
  track.video_width = 100;
  track.video_height = 100;
  track.fill_rects.push_back(
      {10, 20, 30, 40, vr::analysis::OverlayColor{255, 64, 32, 128}});
  package.tracks.push_back(std::move(track));
  return package;
}

}  // namespace

TEST_CASE("analysis overlay geometry follows shared viewport projection",
          "[analysis][overlay][geometry]") {
  const auto geometry = vr::build_analysis_overlay_gpu_geometry(
      package_with_fill(), base_constants(), 200, 100);
  REQUIRE(geometry.fill_rect_count == 1);
  REQUIRE(geometry.line_rect_count == 0);
  REQUIRE(geometry.vertices.size() == 6);
  CHECK(std::abs(geometry.vertices[0].position_x + 0.8f) < 0.0001f);
  CHECK(std::abs(geometry.vertices[0].position_y - 0.6f) < 0.0001f);
  CHECK(std::abs(geometry.vertices[5].position_x + 0.4f) < 0.0001f);
  CHECK(std::abs(geometry.vertices[5].position_y - 0.2f) < 0.0001f);
  CHECK(std::abs(geometry.vertices[0].alpha - 128.0f / 255.0f) < 0.0001f);
}

TEST_CASE("analysis overlay geometry clips each track to its layout cell",
          "[analysis][overlay][geometry]") {
  auto constants = base_constants();
  constants.track_count = 2;
  constants.order[0] = 0;
  constants.order[1] = 1;
  auto package = package_with_fill(0);
  package.tracks[0].fill_rects[0] =
      {0, 0, 100, 100, vr::analysis::OverlayColor{255, 255, 255, 255}};
  const auto geometry = vr::build_analysis_overlay_gpu_geometry(
      package, constants, 200, 100);
  REQUIRE(geometry.vertices.size() == 6);
  for (const auto& vertex : geometry.vertices) {
    CHECK(vertex.position_x <= 0.0001f);
  }
}

TEST_CASE("analysis overlay geometry keeps outline and motion primitives on GPU",
          "[analysis][overlay][geometry]") {
  auto package = package_with_fill();
  auto& track = package.tracks[0];
  track.outline_rects.push_back(
      {5, 5, 20, 20, vr::analysis::OverlayColor{255, 255, 255, 180}});
  track.motion_lines.push_back(
      {10, 10, 90, 90, vr::analysis::OverlayColor{80, 180, 255, 200}});
  const auto geometry = vr::build_analysis_overlay_gpu_geometry(
      package, base_constants(), 200, 100);
  CHECK(geometry.fill_rect_count == 1);
  CHECK(geometry.line_rect_count == 3);
  CHECK(geometry.vertices.size() == 24);
  CHECK(geometry.fill_vertex_count == 6);
  CHECK(geometry.contrast_vertex_count == 12);
  CHECK(geometry.motion_vertex_count == 6);

  const auto& contrast = geometry.vertices[geometry.fill_vertex_count];
  CHECK(contrast.contrast_axis == 1.0f);
  CHECK(contrast.contrast_center_px == Catch::Approx(10.0f));

  const auto px_from_ndc_x = [](float x) { return (x + 1.0f) * 100.0f; };
  CHECK(px_from_ndc_x(geometry.vertices[geometry.fill_vertex_count].position_x) ==
        Catch::Approx(9.0f));
  CHECK(px_from_ndc_x(geometry.vertices[geometry.fill_vertex_count + 1].position_x) ==
        Catch::Approx(12.0f));
}
