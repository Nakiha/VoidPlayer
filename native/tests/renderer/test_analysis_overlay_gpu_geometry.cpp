#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/overlay/analysis_overlay_gpu_geometry.h"

#include <cstdint>
#include <utility>

namespace {

vr::AnalysisOverlayTrackPrimitives base_track() {
  vr::AnalysisOverlayTrackPrimitives track;
  track.slot = 0;
  track.video_width = 100;
  track.video_height = 50;
  return track;
}

}  // namespace

TEST_CASE("analysis overlay GPU primitives remain in normalized source space",
          "[analysis_overlay_gpu_geometry]") {
  vr::AnalysisOverlayPrimitivePackage package;
  auto track = base_track();
  track.fill_rects.push_back(
      {10, 5, 30, 15, vr::analysis::OverlayColor{12, 34, 56, 128}});
  track.outline_rects.push_back(
      {10, 5, 30, 15, vr::analysis::OverlayColor{255, 255, 255, 255}});
  track.motion_lines.push_back(
      {20, 10, 25, 20, vr::analysis::OverlayColor{80, 180, 255, 200}});
  package.tracks.push_back(std::move(track));

  const auto lookup = vr::lookup_analysis_overlay_gpu_primitives(package);
  REQUIRE(lookup.batch);
  CHECK_FALSE(lookup.cache_hit);
  CHECK(lookup.batch->fill_count == 1);
  CHECK(lookup.batch->contrast_count == 2);
  CHECK(lookup.batch->motion_count == 1);
  REQUIRE(lookup.batch->primitives.size() == 4);

  const auto& fill = lookup.batch->primitives[0];
  CHECK(fill.kind == static_cast<uint32_t>(
                         vr::AnalysisOverlayGpuPrimitiveKind::FillRect));
  CHECK(fill.source_uv0[0] == Catch::Approx(0.10f));
  CHECK(fill.source_uv0[1] == Catch::Approx(0.10f));
  CHECK(fill.source_uv1[0] == Catch::Approx(0.30f));
  CHECK(fill.source_uv1[1] == Catch::Approx(0.30f));
  CHECK(fill.color[3] == Catch::Approx(128.0f / 255.0f));

  const auto& vertical = lookup.batch->primitives[1];
  CHECK(vertical.kind == static_cast<uint32_t>(
                             vr::AnalysisOverlayGpuPrimitiveKind::ContrastVertical));
  CHECK(vertical.source_uv0[0] == Catch::Approx(0.10f));
  CHECK(vertical.source_uv1[0] == Catch::Approx(0.10f));
  const auto& horizontal = lookup.batch->primitives[2];
  CHECK(horizontal.kind == static_cast<uint32_t>(
                               vr::AnalysisOverlayGpuPrimitiveKind::ContrastHorizontal));
}

TEST_CASE("analysis overlay GPU primitive cache is independent of layout",
          "[analysis_overlay_gpu_geometry][windows_high_refresh]") {
  vr::AnalysisOverlayPrimitivePackage package;
  package.cache_generation = UINT64_C(0xfedcba9876543210);
  auto track = base_track();
  track.outline_rects.push_back(
      {0, 0, 100, 50, vr::analysis::OverlayColor{255, 255, 255, 255}});
  package.tracks.push_back(std::move(track));

  const auto first = vr::lookup_analysis_overlay_gpu_primitives(package);
  const auto second = vr::lookup_analysis_overlay_gpu_primitives(package);
  REQUIRE(first.batch);
  REQUIRE(second.batch);
  CHECK_FALSE(first.cache_hit);
  CHECK(second.cache_hit);
  CHECK(first.batch == second.batch);
  CHECK(first.batch->contrast_count == 4);
  CHECK(first.batch->line_rect_count() == 4);
}
