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
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace vp_macos {
namespace {

#ifndef VOID_BUILD_ANALYSIS
#define VOID_BUILD_ANALYSIS 0
#endif

bool macos_profiler_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("VOIDPLAYER_MACOS_PROFILER");
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0;
  }();
  return enabled;
}

struct OverlayPrimitiveBuildResult {
  std::vector<VPMacOSNativeOverlayGpuRect> fill_rects;
  std::vector<VPMacOSNativeOverlayGpuRect> line_rects;
  std::vector<VPMacOSNativeOverlayGpuRect> motion_lines;
  uint32_t first_rect_uv0 = 0;
  uint32_t first_rect_uv1 = 0;
  uint32_t first_rect_track_idx = 0;
};

struct OverlayCompositeResult {
  bool expected = false;
  bool applied = false;
  bool gpu_attempted = false;
  bool gpu_succeeded = false;
  int gpu_ret = 0;
  std::string gpu_error;
  bool cpu_attempted = false;
  bool cpu_succeeded = false;
  size_t fill_rect_count = 0;
  size_t line_rect_count = 0;
  size_t motion_line_count = 0;
  uint32_t first_rect_uv0 = 0;
  uint32_t first_rect_uv1 = 0;
  uint32_t first_rect_track_idx = 0;
};

int active_present_frame_count(const vr::RendererDrawSnapshot& snapshot) {
  int count = 0;
  for (const auto& frame : snapshot.decision.frames) {
    if (frame.has_value()) {
      ++count;
    }
  }
  return count;
}

int first_active_present_slot(const vr::RendererDrawSnapshot& snapshot) {
  for (size_t i = 0; i < snapshot.decision.frames.size(); ++i) {
    if (snapshot.decision.frames[i].has_value()) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void log_overlay_composite_result(const char* path,
                                  const vr::RendererDrawSnapshot& snapshot,
                                  const OverlayCompositeResult& result,
                                  int32_t target_width,
                                  int32_t target_height) {
  const bool missed = result.expected && !result.applied;
  if (!missed) {
    return;
  }

  const int slot = first_active_present_slot(snapshot);
  const int file_id = (slot >= 0 && slot < static_cast<int>(snapshot.decision.file_ids.size()))
      ? snapshot.decision.file_ids[static_cast<size_t>(slot)]
      : -1;
  const int64_t pts_us = (slot >= 0 &&
                          slot < static_cast<int>(snapshot.decision.frames.size()) &&
                          snapshot.decision.frames[static_cast<size_t>(slot)].has_value())
      ? snapshot.decision.frames[static_cast<size_t>(slot)]->pts_us
      : snapshot.decision.current_pts_us;

  const char* level_label = missed ? "MISS" : "state";
  spdlog::warn("[MetalOverlay] {} path={} target={}x{} active_frames={} slot={} file_id={} "
               "pts={:.3f}s expected={} applied={} fill_rects={} line_rects={} motion_lines={} "
               "gpu_attempted={} gpu_succeeded={} gpu_ret={} cpu_attempted={} cpu_succeeded={} "
               "layout(mode={}, zoom={:.3f}, offset={:.1f},{:.1f}, pixel_mode={}) "
               "first_rect_uv=0x{:08x}->0x{:08x} track_payload=0x{:08x} gpu_error='{}'",
               level_label,
               path,
               target_width,
               target_height,
               active_present_frame_count(snapshot),
               slot,
               file_id,
               static_cast<double>(pts_us) / 1000000.0,
               result.expected,
               result.applied,
               result.fill_rect_count,
               result.line_rect_count,
               result.motion_line_count,
               result.gpu_attempted,
               result.gpu_succeeded,
               result.gpu_ret,
               result.cpu_attempted,
               result.cpu_succeeded,
               snapshot.layout.mode,
               snapshot.layout.zoom_ratio,
               snapshot.layout.view_offset[0],
               snapshot.layout.view_offset[1],
               snapshot.layout.pixel_size_mode,
               result.first_rect_uv0,
               result.first_rect_uv1,
               result.first_rect_track_idx,
               result.gpu_error);
}

#if VOID_BUILD_ANALYSIS
uint32_t pack_overlay_bgra(vr::analysis::OverlayColor color) {
  return static_cast<uint32_t>(color.b) |
         (static_cast<uint32_t>(color.g) << 8) |
         (static_cast<uint32_t>(color.r) << 16) |
         (static_cast<uint32_t>(color.a) << 24);
}

uint32_t pack_overlay_track_payload(int slot, uint8_t line_alpha) {
  return static_cast<uint32_t>(slot & 0xff) |
         (static_cast<uint32_t>(line_alpha) << 8);
}

OverlayPrimitiveBuildResult build_overlay_primitives_for_metal(
    const vr::RendererDrawSnapshot& snapshot,
    int32_t target_width,
    int32_t target_height) {
  (void)target_width;
  (void)target_height;
  OverlayPrimitiveBuildResult result;
  const auto package = vr::build_analysis_overlay_primitives(snapshot);
  if (package.empty()) {
    return result;
  }

  for (const auto& track : package.tracks) {
    if (track.video_width <= 0 || track.video_height <= 0) {
      continue;
    }
    for (const auto& primitive : track.fill_rects) {
      VPMacOSNativeOverlayGpuRect rect = {};
      rect.rect_uv0 = vr::pack_overlay_uv16(
          primitive.x0, track.video_width, primitive.y0, track.video_height);
      rect.rect_uv1 = vr::pack_overlay_uv16(
          primitive.x1, track.video_width, primitive.y1, track.video_height);
      rect.color_bgra = pack_overlay_bgra(primitive.color);
      rect.track_idx = pack_overlay_track_payload(track.slot, track.line_alpha);
      result.fill_rects.push_back(rect);
    }
    for (const auto& primitive : track.outline_rects) {
      VPMacOSNativeOverlayGpuRect rect = {};
      rect.rect_uv0 = vr::pack_overlay_uv16(
          primitive.x0, track.video_width, primitive.y0, track.video_height);
      rect.rect_uv1 = vr::pack_overlay_uv16(
          primitive.x1, track.video_width, primitive.y1, track.video_height);
      rect.track_idx = pack_overlay_track_payload(track.slot, track.line_alpha);
      if (result.line_rects.empty()) {
        result.first_rect_uv0 = rect.rect_uv0;
        result.first_rect_uv1 = rect.rect_uv1;
        result.first_rect_track_idx = rect.track_idx;
      }
      result.line_rects.push_back(rect);
    }
    for (const auto& line : track.motion_lines) {
      VPMacOSNativeOverlayGpuRect gpu_line = {};
      gpu_line.rect_uv0 = vr::pack_overlay_uv16(
          line.x0, track.video_width, line.y0, track.video_height);
      gpu_line.rect_uv1 = vr::pack_overlay_uv16(
          line.x1, track.video_width, line.y1, track.video_height);
      gpu_line.color_bgra = pack_overlay_bgra(line.color);
      gpu_line.track_idx = pack_overlay_track_payload(track.slot, track.line_alpha);
      result.motion_lines.push_back(gpu_line);
    }
  }
  return result;
}
#else
OverlayPrimitiveBuildResult build_overlay_primitives_for_metal(
    const vr::RendererDrawSnapshot&,
    int32_t,
    int32_t) {
  return {};
}
#endif

bool overlay_primitives_expected(const OverlayPrimitiveBuildResult& overlay) {
  return !overlay.fill_rects.empty() || !overlay.line_rects.empty() ||
         !overlay.motion_lines.empty();
}

VPMacOSNativeOverlayGpuPrimitiveSet overlay_primitive_set(
    const OverlayPrimitiveBuildResult& overlay) {
  VPMacOSNativeOverlayGpuPrimitiveSet set = {};
  set.fill_rects = overlay.fill_rects.empty() ? nullptr : overlay.fill_rects.data();
  set.fill_rect_count = overlay.fill_rects.size();
  set.line_rects = overlay.line_rects.empty() ? nullptr : overlay.line_rects.data();
  set.line_rect_count = overlay.line_rects.size();
  set.motion_lines =
      overlay.motion_lines.empty() ? nullptr : overlay.motion_lines.data();
  set.motion_line_count = overlay.motion_lines.size();
  return set;
}

OverlayCompositeResult overlay_result_from_primitives(
    const OverlayPrimitiveBuildResult& overlay) {
  OverlayCompositeResult result;
  result.fill_rect_count = overlay.fill_rects.size();
  result.line_rect_count = overlay.line_rects.size();
  result.motion_line_count = overlay.motion_lines.size();
  result.expected = overlay_primitives_expected(overlay);
  result.first_rect_uv0 = overlay.first_rect_uv0;
  result.first_rect_uv1 = overlay.first_rect_uv1;
  result.first_rect_track_idx = overlay.first_rect_track_idx;
  return result;
}

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
  stats.overlay_last_expected = overlay_last_expected_ ? 1 : 0;
  stats.overlay_last_applied = overlay_last_applied_ ? 1 : 0;
  stats.overlay_last_line_rect_count = overlay_last_line_rect_count_;
  stats.overlay_expected_count = overlay_expected_count_;
  stats.overlay_applied_count = overlay_applied_count_;
  stats.overlay_missed_count = overlay_missed_count_;
  stats.overlay_gpu_success_count = overlay_gpu_success_count_;
  stats.overlay_gpu_failure_count = overlay_gpu_failure_count_;
  stats.overlay_cpu_fallback_count = overlay_cpu_fallback_count_;
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

void MetalPresentationBackend::record_overlay_result(bool expected,
                                                     bool applied,
                                                     bool gpu_attempted,
                                                     bool gpu_succeeded,
                                                     bool cpu_attempted,
                                                     size_t line_rect_count) {
  overlay_last_expected_ = expected;
  overlay_last_applied_ = applied;
  overlay_last_line_rect_count_ = static_cast<uint64_t>(line_rect_count);
  if (expected) {
    ++overlay_expected_count_;
    if (applied) {
      ++overlay_applied_count_;
    } else {
      ++overlay_missed_count_;
    }
  }
  if (gpu_attempted) {
    if (gpu_succeeded) {
      ++overlay_gpu_success_count_;
    } else {
      ++overlay_gpu_failure_count_;
    }
  }
  if (cpu_attempted) {
    ++overlay_cpu_fallback_count_;
  }
}

OverlayCompositeResult composite_overlay_after_upload(
    const vr::RendererDrawSnapshot& snapshot,
    const vr::PresentationBackendDrawHooks& hooks,
    VPMacOSMetalUploader* uploader,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    const OverlayPrimitiveBuildResult& overlay_primitives,
    bool allow_gpu) {
  OverlayCompositeResult result = overlay_result_from_primitives(overlay_primitives);
  if (!uploader || !pixel_buffer || width <= 0 || height <= 0) {
    return result;
  }
  if (result.expected && allow_gpu) {
    result.gpu_attempted = true;
    char error[256] = {};
    VPMacOSNativePresentDecisionInfo decision = {};
    fill_present_decision_info_from_snapshot(snapshot, width, height, &decision);
    const auto overlay = overlay_primitive_set(overlay_primitives);
    const int ret = VPMacOSMetalUploaderCompositeOverlayGpuPrimitives(
        uploader,
        overlay.fill_rects,
        overlay.fill_rect_count,
        overlay.line_rects,
        overlay.line_rect_count,
        overlay.motion_lines,
        overlay.motion_line_count,
        &decision,
        pixel_buffer,
        width,
        height,
        error,
        sizeof(error));
    result.gpu_ret = ret;
    result.gpu_error = error;
    result.gpu_succeeded = ret == 0;
    if (result.gpu_succeeded) {
      result.applied = true;
      return result;
    }
  }

  if (!result.expected || !hooks.composite_bgra_overlay) {
    return result;
  }
  auto* target = static_cast<CVPixelBufferRef>(pixel_buffer);
  if (CVPixelBufferLockBaseAddress(target, 0) != kCVReturnSuccess) {
    return result;
  }
  auto* bgra = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(target));
  const int pixel_width = static_cast<int>(CVPixelBufferGetWidth(target));
  const int pixel_height = static_cast<int>(CVPixelBufferGetHeight(target));
  const auto stride = static_cast<size_t>(CVPixelBufferGetBytesPerRow(target));
  if (bgra && pixel_width == width && pixel_height == height &&
      stride >= static_cast<size_t>(width) * 4u) {
    result.cpu_attempted = true;
    result.cpu_succeeded =
        hooks.composite_bgra_overlay(snapshot, bgra, width, height, stride);
    result.applied = result.applied || result.cpu_succeeded;
  }
  CVPixelBufferUnlockBaseAddress(target, 0);
  return result;
}

bool MetalPresentationBackend::draw_frame(
    const vr::RendererDrawSnapshot& snapshot,
    const vr::PresentationBackendDrawHooks& hooks) {
  const auto profiler_start = std::chrono::steady_clock::now();
  auto log_profiler = [&](const char* path,
                          bool success,
                          int ret,
                          int64_t copy_us,
                          size_t bytes,
                          int32_t storage,
                          const char* error) {
    if (!macos_profiler_enabled()) {
      return;
    }
    const auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - profiler_start).count();
    ++draw_profiler_count_;
    if (total_us < 8000 && copy_us < 6000 && success && draw_profiler_count_ % 240 != 0) {
      return;
    }
    spdlog::info(
        "[MetalProfiler] draw_frame path={} success={} ret={} total_us={} copy_us={} "
        "bytes={} storage={} target={}x{} slots={} staging_alloc={} staging_reuse={} "
        "overlay_expected={} overlay_applied={} error={}",
        path,
        success,
        ret,
        total_us,
        copy_us,
        bytes,
        storage,
        draw_target_width_,
        draw_target_height_,
        draw_target_max_track_slots_,
        staging_allocation_count_,
        staging_reuse_count_,
        overlay_last_expected_,
        overlay_last_applied_,
        error ? error : "");
  };
  set_last_error("");
  if (!available() || !draw_target_pixel_buffer_ ||
      draw_target_width_ <= 0 || draw_target_height_ <= 0) {
    mark_draw_failure("renderer-owned Metal presentation target is unavailable");
    log_profiler("none", false, -1, 0, 0, 0, last_error_.c_str());
    return false;
  }

  const int32_t track_slots =
      std::clamp(draw_target_max_track_slots_,
                 1,
                 static_cast<int>(VPMacOSNativeMaxTracks));
  const auto overlay_primitives = build_overlay_primitives_for_metal(
      snapshot, draw_target_width_, draw_target_height_);
  const bool overlay_expected = overlay_primitives_expected(overlay_primitives);
  const auto overlay_set = overlay_primitive_set(overlay_primitives);
  const auto storage_extent =
      package_storage_extent(snapshot, draw_target_width_, draw_target_height_);
  const auto package_layout = vr::describe_presentation_package_layout(
      storage_extent.first, storage_extent.second, track_slots);
  if (package_layout.max_bytes == 0 ||
      package_layout.bgra_row_bytes >
          static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    mark_draw_failure("renderer-owned Metal presentation package layout is invalid");
    log_profiler("package-layout", false, -1, 0, package_layout.max_bytes, 0,
                 last_error_.c_str());
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
    int ret = overlay_expected
        ? VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayoutAndOverlay(
              uploader_,
              &cv_frame,
              &overlay_set,
              draw_target_pixel_buffer_,
              draw_target_width_,
              draw_target_height_,
              &frame_info,
              upload_error,
              sizeof(upload_error))
        : VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayout(
              uploader_,
              &cv_frame,
              draw_target_pixel_buffer_,
              draw_target_width_,
              draw_target_height_,
              &frame_info,
              upload_error,
              sizeof(upload_error));
    if (hooks.record_frame_copy_us) {
      hooks.record_frame_copy_us(0);
    }
    if (ret == 0) {
      auto overlay = overlay_result_from_primitives(overlay_primitives);
      if (overlay.expected) {
        overlay.gpu_attempted = true;
        overlay.gpu_succeeded = true;
        overlay.applied = true;
      }
      log_overlay_composite_result("cvpixelbuffer",
                                   snapshot,
                                   overlay,
                                   draw_target_width_,
                                   draw_target_height_);
      record_overlay_result(overlay.expected,
                            overlay.applied,
                            overlay.gpu_attempted,
                            overlay.gpu_succeeded,
                            overlay.cpu_attempted,
                            overlay.line_rect_count);
      mark_draw_success(frame_info);
      log_profiler("cvpixelbuffer", true, ret, 0, 0,
                   VPMacOSNativePresentPackageStorageCVPixelBuffer, "");
      return true;
    }
    if (overlay_expected) {
      OverlayCompositeResult overlay = overlay_result_from_primitives(overlay_primitives);
      overlay.gpu_attempted = true;
      overlay.gpu_ret = ret;
      overlay.gpu_error = upload_error;
      char fallback_error[256] = {};
      ret = VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayout(
          uploader_,
          &cv_frame,
          draw_target_pixel_buffer_,
          draw_target_width_,
          draw_target_height_,
          &frame_info,
          fallback_error,
          sizeof(fallback_error));
      if (ret == 0) {
        auto cpu_overlay = composite_overlay_after_upload(snapshot,
                                                          hooks,
                                                          uploader_,
                                                          draw_target_pixel_buffer_,
                                                          draw_target_width_,
                                                          draw_target_height_,
                                                          overlay_primitives,
                                                          false);
        cpu_overlay.gpu_attempted = true;
        cpu_overlay.gpu_ret = overlay.gpu_ret;
        cpu_overlay.gpu_error = overlay.gpu_error;
        log_overlay_composite_result("cvpixelbuffer-cpu-overlay-fallback",
                                     snapshot,
                                     cpu_overlay,
                                     draw_target_width_,
                                     draw_target_height_);
        record_overlay_result(cpu_overlay.expected,
                              cpu_overlay.applied,
                              cpu_overlay.gpu_attempted,
                              cpu_overlay.gpu_succeeded,
                              cpu_overlay.cpu_attempted,
                              cpu_overlay.line_rect_count);
        mark_draw_success(frame_info);
        log_profiler("cvpixelbuffer-cpu-overlay-fallback", true, ret, 0, 0,
                     VPMacOSNativePresentPackageStorageCVPixelBuffer, "");
        return true;
      }
      if (fallback_error[0]) {
        std::strncpy(upload_error, fallback_error, sizeof(upload_error) - 1);
        upload_error[sizeof(upload_error) - 1] = '\0';
      }
    }
    mark_draw_failure(upload_error[0] ? upload_error : error);
    log_profiler("cvpixelbuffer", false, ret, 0, 0,
                 VPMacOSNativePresentPackageStorageCVPixelBuffer,
                 last_error_.c_str());
    return false;
  }

  VPMacOSNativeCVPixelBufferPresentFrameSet cv_frame_set = {};
  if (snapshot_cv_pixel_buffer_frame_set(snapshot,
                                         draw_target_width_,
                                         draw_target_height_,
                                         &cv_frame_set,
                                         error)) {
    VPMacOSNativeFrameInfo frame_info = {};
    char upload_error[256] = {};
    int ret = overlay_expected
        ? VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameSetWithLayoutAndOverlay(
              uploader_,
              &cv_frame_set,
              &overlay_set,
              draw_target_pixel_buffer_,
              draw_target_width_,
              draw_target_height_,
              &frame_info,
              upload_error,
              sizeof(upload_error))
        : VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameSetWithLayout(
              uploader_,
              &cv_frame_set,
              draw_target_pixel_buffer_,
              draw_target_width_,
              draw_target_height_,
              &frame_info,
              upload_error,
              sizeof(upload_error));
    if (hooks.record_frame_copy_us) {
      hooks.record_frame_copy_us(0);
    }
    if (ret == 0) {
      auto overlay = overlay_result_from_primitives(overlay_primitives);
      if (overlay.expected) {
        overlay.gpu_attempted = true;
        overlay.gpu_succeeded = true;
        overlay.applied = true;
      }
      log_overlay_composite_result("cvpixelbuffer-set",
                                   snapshot,
                                   overlay,
                                   draw_target_width_,
                                   draw_target_height_);
      record_overlay_result(overlay.expected,
                            overlay.applied,
                            overlay.gpu_attempted,
                            overlay.gpu_succeeded,
                            overlay.cpu_attempted,
                            overlay.line_rect_count);
      mark_draw_success(frame_info);
      log_profiler("cvpixelbuffer-set", true, ret, 0, 0,
                   VPMacOSNativePresentPackageStorageCVPixelBuffer, "");
      return true;
    }
    if (overlay_expected) {
      OverlayCompositeResult overlay = overlay_result_from_primitives(overlay_primitives);
      overlay.gpu_attempted = true;
      overlay.gpu_ret = ret;
      overlay.gpu_error = upload_error;
      char fallback_error[256] = {};
      ret = VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameSetWithLayout(
          uploader_,
          &cv_frame_set,
          draw_target_pixel_buffer_,
          draw_target_width_,
          draw_target_height_,
          &frame_info,
          fallback_error,
          sizeof(fallback_error));
      if (ret == 0) {
        auto cpu_overlay = composite_overlay_after_upload(snapshot,
                                                          hooks,
                                                          uploader_,
                                                          draw_target_pixel_buffer_,
                                                          draw_target_width_,
                                                          draw_target_height_,
                                                          overlay_primitives,
                                                          false);
        cpu_overlay.gpu_attempted = true;
        cpu_overlay.gpu_ret = overlay.gpu_ret;
        cpu_overlay.gpu_error = overlay.gpu_error;
        log_overlay_composite_result("cvpixelbuffer-set-cpu-overlay-fallback",
                                     snapshot,
                                     cpu_overlay,
                                     draw_target_width_,
                                     draw_target_height_);
        record_overlay_result(cpu_overlay.expected,
                              cpu_overlay.applied,
                              cpu_overlay.gpu_attempted,
                              cpu_overlay.gpu_succeeded,
                              cpu_overlay.cpu_attempted,
                              cpu_overlay.line_rect_count);
        mark_draw_success(frame_info);
        log_profiler("cvpixelbuffer-set-cpu-overlay-fallback", true, ret, 0, 0,
                     VPMacOSNativePresentPackageStorageCVPixelBuffer, "");
        return true;
      }
      if (fallback_error[0]) {
        std::strncpy(upload_error, fallback_error, sizeof(upload_error) - 1);
        upload_error[sizeof(upload_error) - 1] = '\0';
      }
    }
    mark_draw_failure(upload_error[0] ? upload_error : error);
    log_profiler("cvpixelbuffer-set", false, ret, 0, 0,
                 VPMacOSNativePresentPackageStorageCVPixelBuffer,
                 last_error_.c_str());
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
      log_profiler("package-build", false, -1, 0, data_size, package.storage,
                   last_error_.c_str());
      return false;
    }
    package.storage = VPMacOSNativePresentPackageStorageBGRA;
  }

  VPMacOSNativeFrameInfo frame_info = {};
  char upload_error[256] = {};
  const auto start = std::chrono::steady_clock::now();
  int ret = overlay_expected
      ? VPMacOSMetalUploaderCopyPresentFramePackageWithLayoutAndOverlay(
            uploader_,
            data,
            package.used_bytes,
            &package,
            &overlay_set,
            draw_target_pixel_buffer_,
            draw_target_width_,
            draw_target_height_,
            &frame_info,
            upload_error,
            sizeof(upload_error))
      : VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
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
  const auto copy_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start).count();
  if (hooks.record_frame_copy_us) {
    hooks.record_frame_copy_us(static_cast<uint64_t>(copy_us));
  }
  if (ret == 0) {
    auto overlay = overlay_result_from_primitives(overlay_primitives);
    if (overlay.expected) {
      overlay.gpu_attempted = true;
      overlay.gpu_succeeded = true;
      overlay.applied = true;
    }
    log_overlay_composite_result(
        package.storage == VPMacOSNativePresentPackageStorageYUV ? "package-yuv" : "package-bgra",
        snapshot,
        overlay,
        draw_target_width_,
        draw_target_height_);
    record_overlay_result(overlay.expected,
                          overlay.applied,
                          overlay.gpu_attempted,
                          overlay.gpu_succeeded,
                          overlay.cpu_attempted,
                          overlay.line_rect_count);
    mark_draw_success(frame_info);
    log_profiler(
        package.storage == VPMacOSNativePresentPackageStorageYUV ? "package-yuv" : "package-bgra",
        true,
        ret,
        copy_us,
        package.used_bytes,
        package.storage,
        "");
  } else {
    if (overlay_expected) {
      OverlayCompositeResult overlay = overlay_result_from_primitives(overlay_primitives);
      overlay.gpu_attempted = true;
      overlay.gpu_ret = ret;
      overlay.gpu_error = upload_error;
      char fallback_error[256] = {};
      ret = VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
          uploader_,
          data,
          package.used_bytes,
          &package,
          draw_target_pixel_buffer_,
          draw_target_width_,
          draw_target_height_,
          &frame_info,
          fallback_error,
          sizeof(fallback_error));
      if (ret == 0) {
        auto cpu_overlay = composite_overlay_after_upload(snapshot,
                                                          hooks,
                                                          uploader_,
                                                          draw_target_pixel_buffer_,
                                                          draw_target_width_,
                                                          draw_target_height_,
                                                          overlay_primitives,
                                                          false);
        cpu_overlay.gpu_attempted = true;
        cpu_overlay.gpu_ret = overlay.gpu_ret;
        cpu_overlay.gpu_error = overlay.gpu_error;
        log_overlay_composite_result(
            package.storage == VPMacOSNativePresentPackageStorageYUV
                ? "package-yuv-cpu-overlay-fallback"
                : "package-bgra-cpu-overlay-fallback",
            snapshot,
            cpu_overlay,
            draw_target_width_,
            draw_target_height_);
        record_overlay_result(cpu_overlay.expected,
                              cpu_overlay.applied,
                              cpu_overlay.gpu_attempted,
                              cpu_overlay.gpu_succeeded,
                              cpu_overlay.cpu_attempted,
                              cpu_overlay.line_rect_count);
        mark_draw_success(frame_info);
        log_profiler(
            package.storage == VPMacOSNativePresentPackageStorageYUV
                ? "package-yuv-cpu-overlay-fallback"
                : "package-bgra-cpu-overlay-fallback",
            true,
            ret,
            copy_us,
            package.used_bytes,
            package.storage,
            "");
        return true;
      }
      if (fallback_error[0]) {
        std::strncpy(upload_error, fallback_error, sizeof(upload_error) - 1);
        upload_error[sizeof(upload_error) - 1] = '\0';
      }
    }
    mark_draw_failure(upload_error[0] ? upload_error : error);
    log_profiler(
        package.storage == VPMacOSNativePresentPackageStorageYUV ? "package-yuv" : "package-bgra",
        false,
        ret,
        copy_us,
        package.used_bytes,
        package.storage,
        last_error_.c_str());
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
