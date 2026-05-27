#include "macos/metal_presentation_backend.h"

#include "macos/presentation_package_builder.h"
#include "video_renderer/layout/layout_geometry.h"
#include "video_renderer/render/shader_constants.h"
#include "video_renderer/render/presentation_backend_factory.h"
#include "video_renderer/render/presentation_package.h"

#if VOID_BUILD_ANALYSIS
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

struct TargetRect {
  int x0 = 0;
  int y0 = 0;
  int x1 = 0;
  int y1 = 0;
};

int active_display_count(const vr::ShaderConstants& constants) {
  return std::max(constants.track_count, 1);
}

int display_slot_for_track(const vr::ShaderConstants& constants, int track_slot) {
  const int count = active_display_count(constants);
  for (int i = 0; i < count && i < 4; ++i) {
    if (constants.order[i] == track_slot) {
      return i;
    }
  }
  return -1;
}

bool video_point_to_target(const vr::ShaderConstants& constants,
                           int track_slot,
                           int target_width,
                           int target_height,
                           float video_u,
                           float video_v,
                           float& out_x,
                           float& out_y) {
  if (track_slot < 0 || track_slot >= 4 ||
      constants.inv_display_size_x[track_slot] == 0.0f ||
      constants.inv_display_size_y[track_slot] == 0.0f) {
    return false;
  }
  const float local_x =
      constants.display_offset_x[track_slot] +
      (video_u + constants.view_offset_uv_x[track_slot]) /
          constants.inv_display_size_x[track_slot];
  const float local_y =
      constants.display_offset_y[track_slot] +
      (video_v + constants.view_offset_uv_y[track_slot]) /
          constants.inv_display_size_y[track_slot];
  if (!std::isfinite(local_x) || !std::isfinite(local_y)) {
    return false;
  }

  if (constants.mode == vr::LAYOUT_SPLIT_SCREEN) {
    out_x = local_x * static_cast<float>(target_width);
    out_y = local_y * static_cast<float>(target_height);
    return true;
  }

  const int display_slot = display_slot_for_track(constants, track_slot);
  if (display_slot < 0) {
    return false;
  }
  const int count = active_display_count(constants);
  out_x = (static_cast<float>(display_slot) + local_x) *
          static_cast<float>(target_width) / static_cast<float>(count);
  out_y = local_y * static_cast<float>(target_height);
  return true;
}

bool video_rect_to_target(const vr::ShaderConstants& constants,
                          int track_slot,
                          int target_width,
                          int target_height,
                          int video_width,
                          int video_height,
                          int x0,
                          int y0,
                          int x1,
                          int y1,
                          TargetRect& out) {
  if (video_width <= 0 || video_height <= 0) {
    return false;
  }
  float tx0 = 0.0f;
  float ty0 = 0.0f;
  float tx1 = 0.0f;
  float ty1 = 0.0f;
  if (!video_point_to_target(constants,
                             track_slot,
                             target_width,
                             target_height,
                             static_cast<float>(x0) / static_cast<float>(video_width),
                             static_cast<float>(y0) / static_cast<float>(video_height),
                             tx0,
                             ty0) ||
      !video_point_to_target(constants,
                             track_slot,
                             target_width,
                             target_height,
                             static_cast<float>(x1) / static_cast<float>(video_width),
                             static_cast<float>(y1) / static_cast<float>(video_height),
                             tx1,
                             ty1)) {
    return false;
  }
  out.x0 = static_cast<int>(std::floor(std::min(tx0, tx1)));
  out.y0 = static_cast<int>(std::floor(std::min(ty0, ty1)));
  out.x1 = static_cast<int>(std::ceil(std::max(tx0, tx1)));
  out.y1 = static_cast<int>(std::ceil(std::max(ty0, ty1)));

  if (constants.mode == vr::LAYOUT_SPLIT_SCREEN) {
    const int split_x = static_cast<int>(
        std::lround(constants.split_pos * static_cast<float>(target_width)));
    if (constants.order[0] == track_slot) {
      out.x0 = std::max(out.x0, 0);
      out.x1 = std::min(out.x1, split_x);
    } else if (constants.order[1] == track_slot) {
      out.x0 = std::max(out.x0, split_x);
      out.x1 = std::min(out.x1, target_width);
    } else {
      return false;
    }
  } else {
    const int display_slot = display_slot_for_track(constants, track_slot);
    if (display_slot < 0) {
      return false;
    }
    const int count = active_display_count(constants);
    const int slot_x0 = target_width * display_slot / count;
    const int slot_x1 = target_width * (display_slot + 1) / count;
    out.x0 = std::max(out.x0, slot_x0);
    out.x1 = std::min(out.x1, slot_x1);
  }
  out.y0 = std::max(out.y0, 0);
  out.y1 = std::min(out.y1, target_height);
  return out.x0 < out.x1 && out.y0 < out.y1;
}

void append_line_rect(std::vector<VPMacOSNativeOverlayLineRect>& out,
                      int target_width,
                      int target_height,
                      TargetRect rect) {
  rect.x0 = std::clamp(rect.x0, 0, target_width);
  rect.x1 = std::clamp(rect.x1, 0, target_width);
  rect.y0 = std::clamp(rect.y0, 0, target_height);
  rect.y1 = std::clamp(rect.y1, 0, target_height);
  if (rect.x0 >= rect.x1 || rect.y0 >= rect.y1) {
    return;
  }
  out.push_back({rect.x0, rect.y0, rect.x1, rect.y1});
}

struct OverlayLineBuildResult {
  std::vector<VPMacOSNativeOverlayLineRect> line_rects;
  bool has_cpu_only_primitives = false;
};

#if VOID_BUILD_ANALYSIS
OverlayLineBuildResult build_overlay_line_rects_for_metal(
    const vr::RendererDrawSnapshot& snapshot,
    int32_t target_width,
    int32_t target_height) {
  OverlayLineBuildResult result;
  const auto package = vr::build_analysis_overlay_primitives(snapshot);
  if (package.empty()) {
    return result;
  }

  vr::ShaderConstants constants = {};
  vr::populate_layout_shader_constants(
      constants, snapshot.layout, snapshot.track_geometry, target_width, target_height);

  constexpr int kLineWidth = 2;
  for (const auto& track : package.tracks) {
    result.has_cpu_only_primitives =
        result.has_cpu_only_primitives ||
        !track.fill_rects.empty() ||
        !track.motion_lines.empty();
    if (track.line_alpha == 0 || track.video_width <= 0 || track.video_height <= 0) {
      continue;
    }
    for (const auto& primitive : track.outline_rects) {
      TargetRect rect;
      if (!video_rect_to_target(constants,
                                track.slot,
                                target_width,
                                target_height,
                                track.video_width,
                                track.video_height,
                                primitive.x0,
                                primitive.y0,
                                primitive.x1,
                                primitive.y1,
                                rect)) {
        continue;
      }
      append_line_rect(result.line_rects,
                       target_width,
                       target_height,
                       TargetRect{rect.x0, rect.y0, rect.x1, rect.y0 + kLineWidth});
      append_line_rect(result.line_rects,
                       target_width,
                       target_height,
                       TargetRect{rect.x0, rect.y0, rect.x0 + kLineWidth, rect.y1});
      if (primitive.x1 >= track.video_width) {
        append_line_rect(result.line_rects,
                         target_width,
                         target_height,
                         TargetRect{rect.x1 - kLineWidth, rect.y0, rect.x1, rect.y1});
      }
      if (primitive.y1 >= track.video_height) {
        append_line_rect(result.line_rects,
                         target_width,
                         target_height,
                         TargetRect{rect.x0, rect.y1 - kLineWidth, rect.x1, rect.y1});
      }
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
  const int ret = VPMacOSMetalUploaderCompositeOverlayLineRects(
      uploader,
      overlay.line_rects.data(),
      overlay.line_rects.size(),
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
