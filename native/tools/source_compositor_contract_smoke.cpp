#include "renderer/render/source_compositor_contract.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

bool near(float actual, float expected) {
  return std::fabs(actual - expected) < 0.0001f;
}

}  // namespace

int main() {
  vr::SourceCompositorTrackDescriptor first;
  first.slot = 0;
  first.file_id = 10;
  first.width = 1920;
  first.height = 1080;
  vr::SourceCompositorTrackDescriptor second;
  second.slot = 1;
  second.file_id = 20;
  second.width = 3840;
  second.height = 2160;
  std::vector<vr::SourceCompositorTrackDescriptor> descriptors = {
      first, second};
  const auto policy = vr::resolve_source_compositor_ring_policy(
      descriptors, vr::kSourceCompositorDefaultBudgetBytes);
  if (!policy.allowed || policy.depth != vr::kSourceCompositorLiveBufferCount ||
      policy.frozen_snapshot || policy.bytes_per_frame == 0) {
    return fail("source compositor ring policy rejected normal dual-track budget");
  }

  vr::SourceCompositorTrackDescriptor duplicate;
  duplicate.slot = 1;
  duplicate.file_id = 30;
  duplicate.width = 1280;
  duplicate.height = 720;
  descriptors.push_back(duplicate);
  if (vr::resolve_source_compositor_ring_policy(
          descriptors, vr::kSourceCompositorDefaultBudgetBytes)
          .allowed) {
    return fail("source compositor ring policy accepted duplicate slots");
  }

  vr::SourceCompositorProjection projection;
  projection.enabled = true;
  projection.mode = 0;
  projection.active_track_count = 2;
  projection.source_order = {0, 1, 2, 3};
  projection.display_offset_x[0] = 0.0f;
  projection.display_offset_y[0] = 0.0f;
  projection.inv_display_size_x[0] = 1.0f;
  projection.inv_display_size_y[0] = 1.0f;
  projection.display_offset_x[1] = 0.0f;
  projection.display_offset_y[1] = 0.0f;
  projection.inv_display_size_x[1] = 1.0f;
  projection.inv_display_size_y[1] = 1.0f;

  std::array<bool, vr::kSourceCompositorMaxTracks> present = {
      true, true, false, false};
  const auto left =
      vr::project_source_compositor_sample(0.25f, 0.5f, projection, present);
  const auto right =
      vr::project_source_compositor_sample(0.75f, 0.5f, projection, present);
  if (!left.present || left.source_slot != 0 || !near(left.u, 0.5f) ||
      !right.present || right.source_slot != 1 || !near(right.u, 0.5f)) {
    return fail("source compositor side-by-side projection was not stable");
  }

  projection.mode = 1;
  projection.split_pos = 0.35f;
  const auto rects = vr::project_source_compositor_retained_visuals(
      10.0f, 20.0f, 1010.0f, 520.0f, projection, present);
  if (!rects[0].present || !rects[1].present ||
      !near(rects[0].clip_left, 10.0f) ||
      !near(rects[0].clip_right, 360.0f) ||
      !near(rects[1].clip_left, 360.0f) ||
      !near(rects[1].clip_right, 1010.0f)) {
    return fail("source compositor split visual clipping was not stable");
  }

  return 0;
}
