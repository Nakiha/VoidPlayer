#include "macos/metal/metal_concurrency_policy.h"
#include "macos/metal/metal_layout_params.h"

#include <cstddef>
#include <iostream>

namespace {

int fail(const char* message) {
  std::cerr << message << "\n";
  return 1;
}

}  // namespace

int main() {
  if (sizeof(vp_macos::MetalLayoutParams) != 436) {
    return fail("MetalLayoutParams size drifted from the shader ABI");
  }
  if (offsetof(vp_macos::MetalLayoutParams, overlay_present0) != 404 ||
      offsetof(vp_macos::MetalLayoutParams, background_color_r) != 420) {
    return fail("MetalLayoutParams offsets drifted from the shader ABI");
  }
  if (VPMacOSNativeMaxTracks != 4) {
    return fail("Metal shader ABI requires exactly four native track slots");
  }
  if (vp_macos::kMetalPresentConcurrencyPolicy.max_single_target_in_flight != 1 ||
      vp_macos::kMetalPresentConcurrencyPolicy.max_ring_in_flight != 3 ||
      vp_macos::kMetalPresentConcurrencyPolicy.frame_resource_pool_size != 3) {
    return fail("Metal concurrency policy drifted from the release-readiness contract");
  }
  if (vp_macos::kMetalPresentConcurrencyPolicy.max_ring_in_flight >
      vp_macos::kMetalPresentConcurrencyPolicy.frame_resource_pool_size) {
    return fail("Metal target-ring concurrency exceeds uploader frame resources");
  }
  return 0;
}
