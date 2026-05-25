#pragma once

#include "macos/metal_uploader_bridge.h"
#include "video_renderer/render/renderer_draw_snapshot.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace vp_macos {

void fill_present_decision_info_from_snapshot(
    const vr::RendererDrawSnapshot& snapshot,
    int32_t width,
    int32_t height,
    VPMacOSNativePresentDecisionInfo* out);

bool present_decision_info_is_complete(
    const VPMacOSNativePresentDecisionInfo& info,
    std::string& error);

bool copy_snapshot_yuv_package(const vr::RendererDrawSnapshot& snapshot,
                               uint8_t* dst,
                               size_t dst_size,
                               int32_t width,
                               int32_t height,
                               size_t max_track_slots,
                               VPMacOSNativePresentFramePackageInfo* out,
                               std::string& error);

bool copy_snapshot_bgra_package(const vr::RendererDrawSnapshot& snapshot,
                                uint8_t* dst,
                                size_t dst_size,
                                int32_t width,
                                int32_t height,
                                int32_t stride_bytes,
                                size_t track_stride_bytes,
                                VPMacOSNativePresentFramePackageInfo* out,
                                std::string& error);

bool snapshot_cv_pixel_buffer_frame(const vr::RendererDrawSnapshot& snapshot,
                                    int32_t width,
                                    int32_t height,
                                    VPMacOSNativeCVPixelBufferPresentFrame* out,
                                    std::string& error);

}  // namespace vp_macos
