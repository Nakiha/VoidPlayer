#include "macos/metal_presentation_backend.h"

#include "macos/presentation_package_builder.h"
#include "video_renderer/layout/layout_geometry.h"
#include "video_renderer/render/shader_constants.h"
#include "video_renderer/render/presentation_backend_factory.h"
#include "video_renderer/render/presentation_package.h"

#if VOID_BUILD_ANALYSIS
#include "video_renderer/overlay/analysis_overlay_renderer.h"
#include "video_renderer/overlay/analysis_overlay_primitives.h"
#endif

#include <CoreVideo/CoreVideo.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vp_macos {
namespace {

#ifndef VOID_BUILD_ANALYSIS
#define VOID_BUILD_ANALYSIS 0
#endif

struct OverlayLineBuildResult {
  std::vector<VPMacOSNativeOverlayGpuRect> line_rects;
  bool has_cpu_only_primitives = false;
};

#if VOID_BUILD_ANALYSIS
uint32_t pack_overlay_track_payload(int slot, uint8_t line_alpha) {
  return static_cast<uint32_t>(slot & 0xff) |
         (static_cast<uint32_t>(line_alpha) << 8);
}

OverlayLineBuildResult build_overlay_line_rects_for_metal(
    const vr::RendererDrawSnapshot& snapshot,
    int32_t target_width,
    int32_t target_height) {
  (void)target_width;
  (void)target_height;
  OverlayLineBuildResult result;
  const auto package = vr::build_analysis_overlay_primitives(snapshot);
  if (package.empty()) {
    return result;
  }

  for (const auto& track : package.tracks) {
    result.has_cpu_only_primitives =
        result.has_cpu_only_primitives ||
        !track.fill_rects.empty() ||
        !track.motion_lines.empty();
    if (track.line_alpha == 0 || track.video_width <= 0 || track.video_height <= 0) {
      continue;
    }
    for (const auto& primitive : track.outline_rects) {
      VPMacOSNativeOverlayGpuRect rect = {};
      rect.rect_uv0 = vr::pack_overlay_uv16(
          primitive.x0, track.video_width, primitive.y0, track.video_height);
      rect.rect_uv1 = vr::pack_overlay_uv16(
          primitive.x1, track.video_width, primitive.y1, track.video_height);
      rect.track_idx = pack_overlay_track_payload(track.slot, track.line_alpha);
      result.line_rects.push_back(rect);
    }
  }
  return result;
}
#else
OverlayLineBuildResult build_overlay_line_rects_for_metal(
    const vr::RendererDrawSnapshot&,
    int32_t,
    int32_t) {
  return {};
}
#endif

std::pair<int32_t, int32_t> package_storage_extent(
    const vr::RendererDrawSnapshot& snapshot,
    int32_t target_width,
    int32_t target_height) {
  int32_t width = target_width;
  int32_t height = target_height;
  for (const auto& frame : snapshot.decision.frames) {
    if (!frame.has_value()) {
      continue;
    }
    width = std::max(width, frame->width);
    height = std::max(height, frame->height);
    if (const auto* nv12 = frame->cpu_nv12_storage()) {
      width = std::max(width, nv12->coded_width);
      height = std::max(height, nv12->coded_height);
    }
    if (const auto* planar = frame->cpu_planar_yuv_storage()) {
      width = std::max(width, planar->plane_widths[0]);
      height = std::max(height, planar->plane_heights[0]);
    }
    if (const auto* cv = frame->macos_cv_pixel_buffer_storage()) {
      width = std::max(width, cv->coded_width);
      height = std::max(height, cv->coded_height);
    }
  }
  return {width, height};
}

}  // namespace

MetalPresentationBackend::~MetalPresentationBackend() {
  shutdown();
}

bool MetalPresentationBackend::initialize(const vr::PresentationBackendConfig& config) {
  shutdown();
  width_ = config.width;
  height_ = config.height;
  headless_ = config.headless;
  set_draw_target(config.output,
                  config.width,
                  config.height,
                  config.max_track_slots);
  if (width_ <= 0 || height_ <= 0) {
    return false;
  }
  uploader_ = VPMacOSMetalUploaderCreate();
  return available();
}

void MetalPresentationBackend::shutdown() {
  if (uploader_) {
    VPMacOSMetalUploaderDestroy(uploader_);
    uploader_ = nullptr;
  }
  clear_draw_target();
  width_ = 0;
  height_ = 0;
  headless_ = true;
}

bool MetalPresentationBackend::available() const {
  return uploader_ && VPMacOSMetalUploaderIsAvailable(uploader_) != 0;
}

bool MetalPresentationBackend::update_headless_output(void* output,
                                                      int width,
                                                      int height,
                                                      int max_track_slots) {
  if (!output || width <= 0 || height <= 0) {
    clear_draw_target();
    return false;
  }
  width_ = width;
  height_ = height;
  set_draw_target(output, width, height, max_track_slots);
  return available();
}

void MetalPresentationBackend::clear_headless_output() {
  clear_draw_target();
}

vr::PresentationBackendStats MetalPresentationBackend::presentation_stats() const {
  vr::PresentationBackendStats stats;
  stats.direct_yuv_upload_count =
      VPMacOSMetalUploaderDirectYUVUploadCount(uploader_);
  stats.cvpixelbuffer_upload_count =
      VPMacOSMetalUploaderCVPixelBufferUploadCount(uploader_);
  stats.present_package_upload_count =
      VPMacOSMetalUploaderPresentPackageUploadCount(uploader_);
  stats.last_present_package_copy_us =
      VPMacOSMetalUploaderLastPresentPackageCopyUs(uploader_);
  stats.last_present_package_gpu_wait_us =
      VPMacOSMetalUploaderLastPresentPackageGpuWaitUs(uploader_);
  stats.last_present_package_total_us =
      VPMacOSMetalUploaderLastPresentPackageTotalUs(uploader_);
  stats.last_present_package_storage =
      VPMacOSMetalUploaderLastPresentPackageStorage(uploader_);
  stats.backend_available = available() ? 1 : 0;
  stats.target_installed =
      draw_target_pixel_buffer_ && draw_target_width_ > 0 && draw_target_height_ > 0 ? 1 : 0;
  stats.last_draw_succeeded = last_draw_succeeded_ ? 1 : 0;
  stats.draw_failure_count = draw_failure_count_;
  stats.consecutive_draw_failures = consecutive_draw_failures_;
  stats.last_successful_frame_pts_us = last_draw_frame_info_.pts_us;
  stats.staging_allocation_count = staging_allocation_count_;
  stats.staging_reuse_count = staging_reuse_count_;
  stats.staging_max_bytes = staging_max_bytes_;
  return stats;
}

bool MetalPresentationBackend::copy_last_frame_info(
    vr::PresentationBackendFrameInfo* out) const {
  if (!out || !last_draw_frame_info_available_) {
    return false;
  }
  out->width = last_draw_frame_info_.width;
  out->height = last_draw_frame_info_.height;
  out->pts_us = last_draw_frame_info_.pts_us;
  out->dts_us = last_draw_frame_info_.dts_us;
  out->duration_us = last_draw_frame_info_.duration_us;
  return true;
}

bool MetalPresentationBackend::capture_front_buffer(std::vector<uint8_t>& bgra,
                                                    int& width,
                                                    int& height) {
  auto* pixel_buffer =
      static_cast<CVPixelBufferRef>(draw_target_pixel_buffer_);
  if (!pixel_buffer ||
      CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly) !=
          kCVReturnSuccess) {
    bgra.clear();
    width = 0;
    height = 0;
    return false;
  }
  const int pixel_width = static_cast<int>(CVPixelBufferGetWidth(pixel_buffer));
  const int pixel_height = static_cast<int>(CVPixelBufferGetHeight(pixel_buffer));
  const int stride = static_cast<int>(CVPixelBufferGetBytesPerRow(pixel_buffer));
  const auto* source =
      static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
  if (!source || pixel_width <= 0 || pixel_height <= 0 ||
      stride < pixel_width * 4) {
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    bgra.clear();
    width = 0;
    height = 0;
    return false;
  }
  width = pixel_width;
  height = pixel_height;
  bgra.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  for (int y = 0; y < height; ++y) {
    std::memcpy(bgra.data() + static_cast<size_t>(y) * row_bytes,
                source + static_cast<size_t>(y) * static_cast<size_t>(stride),
                row_bytes);
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  return true;
}

std::unique_ptr<vr::PresentationBackend> create_metal_presentation_backend() {
  return std::make_unique<MetalPresentationBackend>();
}

void MetalPresentationBackend::set_last_error(std::string error) {
  last_error_ = std::move(error);
}

void MetalPresentationBackend::mark_draw_failure(std::string error) {
  last_draw_frame_info_available_ = false;
  last_draw_succeeded_ = false;
  ++draw_failure_count_;
  ++consecutive_draw_failures_;
  set_last_error(std::move(error));
}

void MetalPresentationBackend::mark_draw_success(const VPMacOSNativeFrameInfo& frame_info) {
  last_draw_frame_info_ = frame_info;
  last_draw_frame_info_available_ = true;
  last_draw_succeeded_ = true;
  consecutive_draw_failures_ = 0;
  set_last_error("");
}

bool composite_overlay_lines_with_metal(const vr::RendererDrawSnapshot& snapshot,
                                        VPMacOSMetalUploader* uploader,
                                        void* pixel_buffer,
                                        int32_t width,
                                        int32_t height,
                                        bool& has_cpu_only_primitives) {
  has_cpu_only_primitives = false;
  if (!uploader || !pixel_buffer || width <= 0 || height <= 0) {
    return false;
  }
  const auto overlay = build_overlay_line_rects_for_metal(snapshot, width, height);
  has_cpu_only_primitives = overlay.has_cpu_only_primitives;
  if (overlay.line_rects.empty()) {
    return false;
  }
  char error[256] = {};
  VPMacOSNativePresentDecisionInfo decision = {};
  fill_present_decision_info_from_snapshot(snapshot, width, height, &decision);
  const int ret = VPMacOSMetalUploaderCompositeOverlayGpuRects(
      uploader,
      overlay.line_rects.data(),
      overlay.line_rects.size(),
      &decision,
      pixel_buffer,
      width,
      height,
      error,
      sizeof(error));
  return ret == 0;
}

void composite_overlay_after_upload(const vr::RendererDrawSnapshot& snapshot,
                                    const vr::PresentationBackendDrawHooks& hooks,
                                    VPMacOSMetalUploader* uploader,
                                    void* pixel_buffer,
                                    int32_t width,
                                    int32_t height) {
  if (!pixel_buffer || width <= 0 || height <= 0) {
    return;
  }
  bool has_cpu_only_primitives = false;
  const bool drew_gpu_lines = composite_overlay_lines_with_metal(
      snapshot, uploader, pixel_buffer, width, height, has_cpu_only_primitives);
  if (drew_gpu_lines && !has_cpu_only_primitives) {
    return;
  }
  if (!hooks.composite_bgra_overlay) {
    return;
  }
  auto* target = static_cast<CVPixelBufferRef>(pixel_buffer);
  if (CVPixelBufferLockBaseAddress(target, 0) != kCVReturnSuccess) {
    return;
  }
  auto* bgra = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(target));
  const int pixel_width = static_cast<int>(CVPixelBufferGetWidth(target));
  const int pixel_height = static_cast<int>(CVPixelBufferGetHeight(target));
  const auto stride = static_cast<size_t>(CVPixelBufferGetBytesPerRow(target));
  if (bgra && pixel_width == width && pixel_height == height &&
      stride >= static_cast<size_t>(width) * 4u) {
    (void)hooks.composite_bgra_overlay(snapshot, bgra, width, height, stride);
  }
  CVPixelBufferUnlockBaseAddress(target, 0);
}

bool MetalPresentationBackend::draw_frame(
    const vr::RendererDrawSnapshot& snapshot,
    const vr::PresentationBackendDrawHooks& hooks) {
  set_last_error("");
  if (!available() || !draw_target_pixel_buffer_ ||
      draw_target_width_ <= 0 || draw_target_height_ <= 0) {
    mark_draw_failure("renderer-owned Metal presentation target is unavailable");
    return false;
  }

  const int32_t track_slots =
      std::clamp(draw_target_max_track_slots_,
                 1,
                 static_cast<int>(VPMacOSNativeMaxTracks));
  const auto storage_extent =
      package_storage_extent(snapshot, draw_target_width_, draw_target_height_);
  const auto package_layout = vr::describe_presentation_package_layout(
      storage_extent.first, storage_extent.second, track_slots);
  if (package_layout.max_bytes == 0 ||
      package_layout.bgra_row_bytes >
          static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    mark_draw_failure("renderer-owned Metal presentation package layout is invalid");
    return false;
  }

  std::string error;
  VPMacOSNativeCVPixelBufferPresentFrame cv_frame = {};
  if (snapshot_cv_pixel_buffer_frame(snapshot,
                                     draw_target_width_,
                                     draw_target_height_,
                                     &cv_frame,
                                     error)) {
    VPMacOSNativeFrameInfo frame_info = {};
    char upload_error[256] = {};
    const auto start = std::chrono::steady_clock::now();
    const int ret = VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayout(
        uploader_,
        &cv_frame,
        draw_target_pixel_buffer_,
        draw_target_width_,
        draw_target_height_,
        &frame_info,
        upload_error,
        sizeof(upload_error));
    if (hooks.record_frame_copy_us) {
      hooks.record_frame_copy_us(static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - start).count()));
    }
    if (ret == 0) {
      composite_overlay_after_upload(snapshot,
                                     hooks,
                                     uploader_,
                                     draw_target_pixel_buffer_,
                                     draw_target_width_,
                                     draw_target_height_);
      mark_draw_success(frame_info);
      return true;
    }
    mark_draw_failure(upload_error[0] ? upload_error : error);
    return false;
  }

  VPMacOSNativePresentFramePackageInfo package = {};
  package.width = draw_target_width_;
  package.height = draw_target_height_;
  package.max_track_slots = track_slots;
  fill_present_decision_info_from_snapshot(
      snapshot, draw_target_width_, draw_target_height_, &package.decision);

  if (staging_buffer_.size() < package_layout.max_bytes) {
    staging_buffer_.assign(package_layout.max_bytes, 0);
    ++staging_allocation_count_;
    staging_max_bytes_ = std::max(staging_max_bytes_, staging_buffer_.size());
  } else {
    ++staging_reuse_count_;
  }
  auto* data = staging_buffer_.data();
  const auto data_size = staging_buffer_.size();
  if (copy_snapshot_yuv_package(snapshot,
                                data,
                                data_size,
                                draw_target_width_,
                                draw_target_height_,
                                static_cast<size_t>(track_slots),
                                &package,
                                error)) {
    package.storage = VPMacOSNativePresentPackageStorageYUV;
  } else {
    fill_present_decision_info_from_snapshot(
        snapshot, draw_target_width_, draw_target_height_, &package.decision);
    package.stride_bytes = static_cast<int32_t>(package_layout.bgra_row_bytes);
    package.track_stride_bytes = package_layout.bgra_track_stride_bytes;
    if (!copy_snapshot_bgra_package(snapshot,
                                    data,
                                    data_size,
                                    draw_target_width_,
                                    draw_target_height_,
                                    package.stride_bytes,
                                    package.track_stride_bytes,
                                    &package,
                                    error)) {
      mark_draw_failure(error);
      return false;
    }
    package.storage = VPMacOSNativePresentPackageStorageBGRA;
  }

  VPMacOSNativeFrameInfo frame_info = {};
  char upload_error[256] = {};
  const auto start = std::chrono::steady_clock::now();
  const int ret = VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
      uploader_,
      data,
      package.used_bytes,
      &package,
      draw_target_pixel_buffer_,
      draw_target_width_,
      draw_target_height_,
      &frame_info,
      upload_error,
      sizeof(upload_error));
  if (hooks.record_frame_copy_us) {
    hooks.record_frame_copy_us(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count()));
  }
  if (ret == 0) {
    composite_overlay_after_upload(snapshot,
                                   hooks,
                                   uploader_,
                                   draw_target_pixel_buffer_,
                                   draw_target_width_,
                                   draw_target_height_);
    mark_draw_success(frame_info);
  } else {
    mark_draw_failure(upload_error[0] ? upload_error : error);
  }
  return ret == 0;
}

void MetalPresentationBackend::set_draw_target(void* pixel_buffer,
                                               int32_t width,
                                               int32_t height,
                                               int32_t max_track_slots) {
  draw_target_pixel_buffer_ = pixel_buffer;
  draw_target_width_ = width;
  draw_target_height_ = height;
  draw_target_max_track_slots_ =
      std::clamp(max_track_slots,
                 1,
                 static_cast<int32_t>(VPMacOSNativeMaxTracks));
  last_draw_succeeded_ = false;
}

void MetalPresentationBackend::clear_draw_target() {
  draw_target_pixel_buffer_ = nullptr;
  draw_target_width_ = 0;
  draw_target_height_ = 0;
  draw_target_max_track_slots_ = VPMacOSNativeMaxTracks;
  last_draw_frame_info_available_ = false;
  last_draw_frame_info_ = {};
  last_draw_succeeded_ = false;
  set_last_error("");
}

bool MetalPresentationBackend::copy_last_draw_frame_info(
    VPMacOSNativeFrameInfo* out) const {
  if (!out || !last_draw_frame_info_available_) {
    return false;
  }
  *out = last_draw_frame_info_;
  return true;
}

}  // namespace vp_macos

namespace vr {

namespace {

class MetalPresentationBackendProvider final : public PresentationBackendProvider {
public:
  bool supports(RenderBackendKind kind) const override {
    return kind == RenderBackendKind::Metal;
  }

  std::unique_ptr<PresentationBackend> create(RenderBackendKind kind) const override {
    if (!supports(kind)) {
      return nullptr;
    }
    return vp_macos::create_metal_presentation_backend();
  }
};

}  // namespace

const PresentationBackendProvider* default_presentation_backend_provider() {
  static const MetalPresentationBackendProvider provider;
  return &provider;
}

std::unique_ptr<PresentationBackend> create_presentation_backend(
    RenderBackendKind kind) {
  const auto* provider = default_presentation_backend_provider();
  return provider && provider->supports(kind) ? provider->create(kind) : nullptr;
}

}  // namespace vr
