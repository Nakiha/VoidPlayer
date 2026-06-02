#pragma once

#include <cstddef>
#include <cstdint>

namespace vr {

struct PresentationPackageLayout {
  size_t bgra_row_bytes = 0;
  size_t bgra_track_stride_bytes = 0;
  size_t bgra_max_bytes = 0;
  size_t yuv_max_bytes = 0;
  size_t max_bytes = 0;
};

PresentationPackageLayout describe_presentation_package_layout(int32_t width,
                                                               int32_t height,
                                                               int32_t track_slots);

}  // namespace vr
