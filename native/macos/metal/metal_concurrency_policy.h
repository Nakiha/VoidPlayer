#ifndef VOIDPLAYER_MACOS_METAL_CONCURRENCY_POLICY_H_
#define VOIDPLAYER_MACOS_METAL_CONCURRENCY_POLICY_H_

#include <cstddef>
#include <cstdint>

namespace vp_macos {

struct MetalPresentConcurrencyPolicy {
  uint32_t max_single_target_in_flight = 1;
  uint32_t max_ring_in_flight = 3;
  size_t frame_resource_pool_size = 3;
};

constexpr MetalPresentConcurrencyPolicy kMetalPresentConcurrencyPolicy{};

static_assert(kMetalPresentConcurrencyPolicy.max_single_target_in_flight == 1,
              "single CVPixelBuffer targets must remain serialized");
static_assert(kMetalPresentConcurrencyPolicy.max_ring_in_flight <=
                  kMetalPresentConcurrencyPolicy.frame_resource_pool_size,
              "target-ring concurrency must fit the uploader resource pool");

}  // namespace vp_macos

#endif  // VOIDPLAYER_MACOS_METAL_CONCURRENCY_POLICY_H_
