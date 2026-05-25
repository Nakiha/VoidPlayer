#include "macos/presentation_package_builder.h"

#include <string>

namespace vp_macos {

bool snapshot_cv_pixel_buffer_frame(const vr::RendererDrawSnapshot& snapshot,
                                    int32_t width,
                                    int32_t height,
                                    VPMacOSNativeCVPixelBufferPresentFrame* out,
                                    std::string& error) {
  if (!out || width <= 0 || height <= 0) {
    error = "invalid CVPixelBuffer snapshot output";
    return false;
  }
  *out = {};
  fill_present_decision_info_from_snapshot(snapshot, width, height, &out->decision);
  if (!present_decision_info_is_complete(out->decision, error)) {
    return false;
  }
  if (out->decision.frame_count != 1) {
    error = "snapshot is not a single CVPixelBuffer frame";
    return false;
  }
  for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
    if (!snapshot.decision.frames[slot].has_value()) {
      continue;
    }
    if (slot != 0) {
      error = "CVPixelBuffer fast path currently requires the primary track slot";
      return false;
    }
    const auto& frame = *snapshot.decision.frames[slot];
    const auto* storage = frame.macos_cv_pixel_buffer_storage();
    if (!storage || !storage->pixel_buffer || storage->plane_count < 2 ||
        storage->coded_width < frame.width || storage->coded_height < frame.height) {
      error = "snapshot does not contain a supported CVPixelBuffer frame";
      return false;
    }
    out->pixel_buffer = storage->pixel_buffer;
    out->pixel_format = static_cast<int32_t>(storage->pixel_format);
    out->plane_count = storage->plane_count;
    out->is_p010 = storage->is_p010 ? 1 : 0;
    out->coded_width = storage->coded_width;
    out->coded_height = storage->coded_height;
    out->decision.yuv_format[slot] = storage->is_p010
        ? VPMacOSNativePresentFormatP010
        : VPMacOSNativePresentFormatNV12;
    out->decision.coded_width[slot] = storage->coded_width;
    out->decision.coded_height[slot] = storage->coded_height;
    out->decision.nv12_uv_scale_x[slot] =
        static_cast<float>(frame.width) / static_cast<float>(storage->coded_width);
    out->decision.nv12_uv_scale_y[slot] =
        static_cast<float>(frame.height) / static_cast<float>(storage->coded_height);
    return true;
  }
  error = "snapshot has no CVPixelBuffer frame";
  return false;
}

}  // namespace vp_macos
