#include "video_renderer/render/presentation_package.h"

#include <algorithm>
#include <limits>

namespace vr {
namespace {

bool checked_mul_size(size_t lhs, size_t rhs, size_t& out) {
  if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
    return false;
  }
  out = lhs * rhs;
  return true;
}

size_t bgra_package_bytes(int32_t width,
                          int32_t height,
                          int32_t track_slots,
                          size_t* row_bytes,
                          size_t* track_stride_bytes) {
  if (width <= 0 || height <= 0 || track_slots <= 0) {
    return 0;
  }
  size_t row = 0;
  size_t track = 0;
  size_t total = 0;
  if (!checked_mul_size(static_cast<size_t>(width), 4u, row) ||
      !checked_mul_size(row, static_cast<size_t>(height), track) ||
      !checked_mul_size(track, static_cast<size_t>(track_slots), total)) {
    return 0;
  }
  if (row_bytes) {
    *row_bytes = row;
  }
  if (track_stride_bytes) {
    *track_stride_bytes = track;
  }
  return total;
}

size_t yuv_package_bytes(int32_t width, int32_t height, int32_t track_slots) {
  if (width <= 0 || height <= 0 || track_slots <= 0) {
    return 0;
  }
  const auto coded_width = static_cast<size_t>((width + 1) & ~1);
  const auto coded_height = static_cast<size_t>((height + 1) & ~1);
  size_t plane_pixels = 0;
  size_t p010_bytes = 0;
  size_t total = 0;
  if (!checked_mul_size(coded_width, coded_height, plane_pixels) ||
      !checked_mul_size(plane_pixels, 3u, p010_bytes) ||
      !checked_mul_size(p010_bytes, static_cast<size_t>(track_slots), total)) {
    return 0;
  }
  return total;
}

}  // namespace

PresentationPackageLayout describe_presentation_package_layout(int32_t width,
                                                               int32_t height,
                                                               int32_t track_slots) {
  PresentationPackageLayout layout;
  layout.bgra_max_bytes = bgra_package_bytes(
      width, height, track_slots, &layout.bgra_row_bytes,
      &layout.bgra_track_stride_bytes);
  layout.yuv_max_bytes = yuv_package_bytes(width, height, track_slots);
  layout.max_bytes = std::max(layout.bgra_max_bytes, layout.yuv_max_bytes);
  return layout;
}

}  // namespace vr
