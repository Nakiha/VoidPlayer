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

  const auto sdr_output = vr::source_compositor_sdr_output_contract();
  const auto edr_output = vr::source_compositor_edr_output_contract();
  if (!vr::validate_source_compositor_output_contract(sdr_output) ||
      !vr::validate_source_compositor_output_contract(edr_output) ||
      sdr_output.extended_range || !edr_output.extended_range) {
    return fail("source compositor output contracts were not deterministic");
  }

  vr::SourceCompositorPackageMetadata package;
  package.output = edr_output;
  package.topology_generation = 1;
  package.ring_generation = 2;
  package.frame_generation = 3;
  package.buffer_index = 1;
  package.ring_depth = vr::kSourceCompositorLiveBufferCount;
  package.track_count = descriptors.size();
  package.required_slot_mask = 0b11;
  package.published_slot_mask = 0b11;
  if (!vr::validate_source_compositor_package(package, descriptors)) {
    return fail("source compositor rejected a complete package");
  }
  package.published_slot_mask = 0b01;
  if (vr::validate_source_compositor_package(package, descriptors)) {
    return fail("source compositor accepted an incomplete package");
  }
  package.published_slot_mask = 0b11;

  vr::SourceCompositorLifecycle lifecycle;
  vr::SourceCompositorLifecycleEvent event;
  event.type = vr::SourceCompositorLifecycleEventType::BeginAllocation;
  event.topology_generation = 1;
  if (!vr::apply_source_compositor_lifecycle_event(lifecycle, event) ||
      lifecycle.state != vr::SourceCompositorLifecycleState::Allocating) {
    return fail("source compositor did not enter allocating state");
  }
  event.type = vr::SourceCompositorLifecycleEventType::MarkReady;
  event.ring_generation = 2;
  if (!vr::apply_source_compositor_lifecycle_event(lifecycle, event) ||
      lifecycle.state != vr::SourceCompositorLifecycleState::Ready) {
    return fail("source compositor did not enter ready state");
  }
  event.type = vr::SourceCompositorLifecycleEventType::Publish;
  event.frame_generation = 3;
  if (!vr::apply_source_compositor_lifecycle_event(lifecycle, event) ||
      lifecycle.state != vr::SourceCompositorLifecycleState::Publishing ||
      lifecycle.publish_count != 1) {
    return fail("source compositor did not publish a valid generation");
  }
  if (vr::apply_source_compositor_lifecycle_event(lifecycle, event)) {
    return fail("source compositor accepted a duplicate frame generation");
  }
  event.type = vr::SourceCompositorLifecycleEventType::BeginDraining;
  if (!vr::apply_source_compositor_lifecycle_event(lifecycle, event) ||
      lifecycle.state != vr::SourceCompositorLifecycleState::Draining) {
    return fail("source compositor did not enter draining state");
  }
  event = vr::SourceCompositorLifecycleEvent();
  if (!vr::apply_source_compositor_lifecycle_event(lifecycle, event) ||
      lifecycle.state != vr::SourceCompositorLifecycleState::Unconfigured) {
    return fail("source compositor did not reset");
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
