#include "macos/metal/metal_presentation_backend.h"

#include "macos/metal/metal_concurrency_policy.h"
#include "macos/presentation/presentation_package_builder.h"
#include "renderer/layout/layout_geometry.h"
#include "renderer/render/shader_constants.h"
#include "renderer/render/presentation_package.h"

#if VOID_BUILD_ANALYSIS
#include "renderer/overlay/analysis_overlay_renderer.h"
#include "renderer/overlay/analysis_overlay_primitives.h"
#endif

#include <CoreVideo/CoreVideo.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
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

uint64_t percentile_95_us(std::vector<uint64_t> samples) {
  if (samples.empty()) {
    return 0;
  }
  std::sort(samples.begin(), samples.end());
  const auto index = std::min(
      samples.size() - 1,
      static_cast<size_t>(
          std::ceil(static_cast<double>(samples.size()) * 0.95) - 1.0));
  return samples[index];
}

struct OverlayPrimitiveBuildResult {
  uint64_t generation = 0;
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

#if VOID_BUILD_ANALYSIS
struct MetalOverlayPrimitiveCacheKey {
  const void* package = nullptr;
  uint64_t package_generation = 0;

  bool operator==(const MetalOverlayPrimitiveCacheKey& other) const {
    return package == other.package &&
           package_generation == other.package_generation;
  }
};

struct MetalOverlayPrimitiveCacheEntry {
  MetalOverlayPrimitiveCacheKey key;
  std::shared_ptr<const OverlayPrimitiveBuildResult> result;
  uint64_t last_used = 0;
};
#endif

struct AsyncDrawContext {
  MetalPresentationBackend* backend = nullptr;
  vr::PresentationBackendDrawHooks hooks;
  OverlayCompositeResult overlay;
  MetalPresentationBackend::DrawTicket ticket;
  const char* path = "unknown";
  int32_t storage = VPMacOSNativePresentPackageStorageUnavailable;
  int64_t copy_us = 0;
  size_t bytes = 0;
  uint64_t target_pixel_buffer_address = 0;
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

void hash_combine(uint64_t& seed, uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

uint64_t pointer_bits(const void* ptr) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr));
}

void* pointer_from_bits(uint64_t bits) {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(bits));
}

void annotate_frame_target(VPMacOSNativeFrameInfo* frame_info,
                           const void* pixel_buffer) {
  if (!frame_info) {
    return;
  }
  frame_info->target_pixel_buffer_address = pointer_bits(pixel_buffer);
}

uint64_t source_frame_signature(const vr::RendererDrawSnapshot& snapshot,
                                int32_t target_width,
                                int32_t target_height,
                                int32_t track_slots) {
  uint64_t hash = 1469598103934665603ull;
  hash_combine(hash, snapshot.decision.should_present ? 1u : 0u);
  hash_combine(hash, static_cast<uint64_t>(target_width));
  hash_combine(hash, static_cast<uint64_t>(target_height));
  hash_combine(hash, static_cast<uint64_t>(track_slots));
  for (size_t slot = 0; slot < snapshot.decision.frames.size(); ++slot) {
    const auto& frame = snapshot.decision.frames[slot];
    hash_combine(hash, static_cast<uint64_t>(slot));
    hash_combine(hash, static_cast<uint64_t>(
        snapshot.decision.file_ids[slot] < 0 ? 0 : snapshot.decision.file_ids[slot] + 1));
    hash_combine(hash, snapshot.decision.track_generations[slot]);
    if (!frame.has_value()) {
      hash_combine(hash, 0);
      continue;
    }
    hash_combine(hash, 1);
    hash_combine(hash, static_cast<uint64_t>(frame->pts_us));
    hash_combine(hash, static_cast<uint64_t>(frame->dts_us));
    hash_combine(hash, static_cast<uint64_t>(frame->duration_us));
    hash_combine(hash, static_cast<uint64_t>(frame->width));
    hash_combine(hash, static_cast<uint64_t>(frame->height));
    hash_combine(hash, static_cast<uint64_t>(frame->storage_kind()));
    if (const auto* rgba = frame->cpu_rgba_storage()) {
      hash_combine(hash, pointer_bits(rgba->data.get()));
      hash_combine(hash, static_cast<uint64_t>(rgba->stride));
    } else if (const auto* nv12 = frame->cpu_nv12_storage()) {
      hash_combine(hash, pointer_bits(nv12->data.get()));
      hash_combine(hash, static_cast<uint64_t>(nv12->y_stride));
      hash_combine(hash, static_cast<uint64_t>(nv12->uv_stride));
      hash_combine(hash, nv12->is_p010 ? 1u : 0u);
      hash_combine(hash, static_cast<uint64_t>(nv12->coded_width));
      hash_combine(hash, static_cast<uint64_t>(nv12->coded_height));
    } else if (const auto* planar = frame->cpu_planar_yuv_storage()) {
      for (int plane = 0; plane < 3; ++plane) {
        hash_combine(hash, pointer_bits(planar->planes[plane]));
        hash_combine(hash, static_cast<uint64_t>(planar->strides[plane]));
        hash_combine(hash, static_cast<uint64_t>(planar->plane_widths[plane]));
        hash_combine(hash, static_cast<uint64_t>(planar->plane_heights[plane]));
      }
      hash_combine(hash, static_cast<uint64_t>(planar->bytes_per_sample));
    } else if (const auto* cv_pixel = frame->cv_pixel_buffer_storage()) {
      hash_combine(hash, pointer_bits(cv_pixel->pixel_buffer));
      hash_combine(hash, static_cast<uint64_t>(cv_pixel->pixel_format));
      hash_combine(hash, static_cast<uint64_t>(cv_pixel->plane_count));
      hash_combine(hash, cv_pixel->is_p010 ? 1u : 0u);
      hash_combine(hash, static_cast<uint64_t>(cv_pixel->coded_width));
      hash_combine(hash, static_cast<uint64_t>(cv_pixel->coded_height));
    }
  }
  return hash;
}

void update_cached_package_layout_decision(
    const vr::RendererDrawSnapshot& snapshot,
    int32_t width,
    int32_t height,
    VPMacOSNativePresentFramePackageInfo* package) {
  if (!package) {
    return;
  }
  VPMacOSNativePresentDecisionInfo next_decision = {};
  fill_present_decision_info_from_snapshot(snapshot, width, height, &next_decision);
  for (size_t slot = 0; slot < vr::kMaxTracks; ++slot) {
    next_decision.y_offset[slot] = package->decision.y_offset[slot];
    next_decision.uv_offset[slot] = package->decision.uv_offset[slot];
    next_decision.v_offset[slot] = package->decision.v_offset[slot];
    next_decision.y_stride[slot] = package->decision.y_stride[slot];
    next_decision.uv_stride[slot] = package->decision.uv_stride[slot];
    next_decision.coded_width[slot] = package->decision.coded_width[slot];
    next_decision.coded_height[slot] = package->decision.coded_height[slot];
    next_decision.yuv_format[slot] = package->decision.yuv_format[slot];
    if (package->decision.nv12_uv_scale_x[slot] > 0.0f) {
      next_decision.nv12_uv_scale_x[slot] =
          package->decision.nv12_uv_scale_x[slot];
    }
    if (package->decision.nv12_uv_scale_y[slot] > 0.0f) {
      next_decision.nv12_uv_scale_y[slot] =
          package->decision.nv12_uv_scale_y[slot];
    }
  }
  package->decision = next_decision;
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

std::shared_ptr<const OverlayPrimitiveBuildResult> empty_metal_overlay_primitives() {
  static const auto empty = std::make_shared<const OverlayPrimitiveBuildResult>();
  return empty;
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

constexpr size_t kMetalOverlayPrimitiveCacheLimit = 24;

std::mutex& metal_overlay_primitive_cache_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<MetalOverlayPrimitiveCacheEntry>& metal_overlay_primitive_cache_entries() {
  static std::vector<MetalOverlayPrimitiveCacheEntry> entries;
  return entries;
}

uint64_t& metal_overlay_primitive_cache_clock() {
  static uint64_t clock = 0;
  return clock;
}

std::shared_ptr<const OverlayPrimitiveBuildResult> lookup_metal_overlay_primitives(
    MetalOverlayPrimitiveCacheKey key) {
  std::lock_guard<std::mutex> lock(metal_overlay_primitive_cache_mutex());
  auto& clock = metal_overlay_primitive_cache_clock();
  const uint64_t use_token = ++clock;
  for (auto& entry : metal_overlay_primitive_cache_entries()) {
    if (entry.key == key) {
      entry.last_used = use_token;
      return entry.result;
    }
  }
  return nullptr;
}

void store_metal_overlay_primitives(
    MetalOverlayPrimitiveCacheKey key,
    std::shared_ptr<const OverlayPrimitiveBuildResult> result) {
  if (!result) {
    return;
  }
  std::lock_guard<std::mutex> lock(metal_overlay_primitive_cache_mutex());
  auto& clock = metal_overlay_primitive_cache_clock();
  auto& entries = metal_overlay_primitive_cache_entries();
  const uint64_t use_token = ++clock;
  for (auto& entry : entries) {
    if (entry.key == key) {
      entry.result = std::move(result);
      entry.last_used = use_token;
      return;
    }
  }
  if (entries.size() >= kMetalOverlayPrimitiveCacheLimit) {
    const auto oldest = std::min_element(
        entries.begin(),
        entries.end(),
        [](const auto& lhs, const auto& rhs) {
          return lhs.last_used < rhs.last_used;
        });
    if (oldest != entries.end()) {
      entries.erase(oldest);
    }
  }
  entries.push_back(
      MetalOverlayPrimitiveCacheEntry{key, std::move(result), use_token});
}

std::shared_ptr<const OverlayPrimitiveBuildResult> build_overlay_primitives_for_metal(
    const vr::RendererDrawSnapshot& snapshot,
    int32_t target_width,
    int32_t target_height) {
  (void)target_width;
  (void)target_height;
  const auto package = vr::build_analysis_overlay_primitive_package(snapshot);
  if (!package || package->empty()) {
    return empty_metal_overlay_primitives();
  }

  const MetalOverlayPrimitiveCacheKey cache_key{
      package.get(),
      package->cache_generation,
  };
  if (const auto cached = lookup_metal_overlay_primitives(cache_key)) {
    return cached;
  }

  auto result = std::make_shared<OverlayPrimitiveBuildResult>();
  result->generation = package->cache_generation;
  for (const auto& track : package->tracks) {
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
      result->fill_rects.push_back(rect);
    }
    for (const auto& primitive : track.outline_rects) {
      VPMacOSNativeOverlayGpuRect rect = {};
      rect.rect_uv0 = vr::pack_overlay_uv16(
          primitive.x0, track.video_width, primitive.y0, track.video_height);
      rect.rect_uv1 = vr::pack_overlay_uv16(
          primitive.x1, track.video_width, primitive.y1, track.video_height);
      rect.track_idx = pack_overlay_track_payload(track.slot, track.line_alpha);
      if (result->line_rects.empty()) {
        result->first_rect_uv0 = rect.rect_uv0;
        result->first_rect_uv1 = rect.rect_uv1;
        result->first_rect_track_idx = rect.track_idx;
      }
      result->line_rects.push_back(rect);
    }
    for (const auto& line : track.motion_lines) {
      VPMacOSNativeOverlayGpuRect gpu_line = {};
      gpu_line.rect_uv0 = vr::pack_overlay_uv16(
          line.x0, track.video_width, line.y0, track.video_height);
      gpu_line.rect_uv1 = vr::pack_overlay_uv16(
          line.x1, track.video_width, line.y1, track.video_height);
      gpu_line.color_bgra = pack_overlay_bgra(line.color);
      gpu_line.track_idx = pack_overlay_track_payload(track.slot, track.line_alpha);
      result->motion_lines.push_back(gpu_line);
    }
  }
  store_metal_overlay_primitives(cache_key, result);
  return result;
}
#else
std::shared_ptr<const OverlayPrimitiveBuildResult> build_overlay_primitives_for_metal(
    const vr::RendererDrawSnapshot&,
    int32_t,
    int32_t) {
  static const auto empty = std::make_shared<const OverlayPrimitiveBuildResult>();
  return empty;
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
  set.generation = overlay.generation;
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
    if (const auto* cv = frame->cv_pixel_buffer_storage()) {
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

const char* MetalPresentationBackend::last_error() const {
  thread_local std::string error_copy;
  std::lock_guard<std::mutex> lock(async_mutex_);
  error_copy = last_error_;
  return error_copy.c_str();
}

bool MetalPresentationBackend::initialize(const vr::PresentationBackendConfig& config) {
  shutdown();
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    async_shutdown_ = false;
  }
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
  {
    std::unique_lock<std::mutex> lock(async_mutex_);
    async_shutdown_ = true;
    async_cv_.wait(lock, [this] { return in_flight_draws_ == 0; });
  }
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
  char validation_error[256] = {};
  if (VPMacOSMetalUploaderValidatePixelBufferChecked(uploader_,
                                                     output,
                                                     width,
                                                     height,
                                                     validation_error,
                                                     sizeof(validation_error)) != 0) {
    mark_draw_failure(validation_error[0] != '\0'
                          ? validation_error
                          : "renderer-owned Metal presentation target is invalid");
    return false;
  }
  width_ = width;
  height_ = height;
  set_draw_target(output, width, height, max_track_slots);
  return available();
}

bool MetalPresentationBackend::update_headless_output_ring(
    const void* const* pixel_buffers,
    size_t pixel_buffer_count,
    void* displayed_pixel_buffer,
    void* protected_pixel_buffer,
    int width,
    int height,
    int max_track_slots) {
  if (!pixel_buffers || pixel_buffer_count == 0 || width <= 0 || height <= 0) {
    clear_draw_target();
    return false;
  }
  for (size_t i = 0; i < pixel_buffer_count; ++i) {
    char validation_error[256] = {};
    if (!pixel_buffers[i] ||
        VPMacOSMetalUploaderValidatePixelBufferChecked(
            uploader_,
            const_cast<void*>(pixel_buffers[i]),
            width,
            height,
            validation_error,
            sizeof(validation_error)) != 0) {
      mark_draw_failure(validation_error[0] != '\0'
                            ? validation_error
                            : "renderer-owned Metal presentation target ring is invalid");
      return false;
    }
  }
  width_ = width;
  height_ = height;
  set_draw_target_ring(pixel_buffers,
                       pixel_buffer_count,
                       displayed_pixel_buffer,
                       protected_pixel_buffer,
                       width,
                       height,
                       max_track_slots);
  return available();
}

void MetalPresentationBackend::clear_headless_output() {
  clear_draw_target();
}

void MetalPresentationBackend::mark_headless_output_displayed(void* pixel_buffer) {
  mark_displayed_target(pixel_buffer);
}

void MetalPresentationBackend::protect_headless_output(void* pixel_buffer) {
  protect_target(pixel_buffer);
}

void MetalPresentationBackend::release_headless_output(void* pixel_buffer) {
  release_target(pixel_buffer);
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
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    stats.target_installed =
        ((draw_target_pixel_buffer_ || target_ring_enabled_) &&
         draw_target_width_ > 0 && draw_target_height_ > 0)
            ? 1
            : 0;
  }
  stats.last_draw_succeeded = last_draw_succeeded_ ? 1 : 0;
  stats.draw_failure_count = draw_failure_count_;
  stats.consecutive_draw_failures = consecutive_draw_failures_;
  stats.last_successful_frame_pts_us = last_draw_frame_info_.pts_us;
  stats.staging_allocation_count = staging_allocation_count_;
  stats.staging_reuse_count = staging_reuse_count_;
  stats.staging_max_bytes = staging_max_bytes_;
  stats.overlay_last_expected = overlay_last_expected_ ? 1 : 0;
  stats.overlay_last_applied = overlay_last_applied_ ? 1 : 0;
  stats.overlay_last_fill_rect_count = overlay_last_fill_rect_count_;
  stats.overlay_last_line_rect_count = overlay_last_line_rect_count_;
  stats.overlay_expected_count = overlay_expected_count_;
  stats.overlay_applied_count = overlay_applied_count_;
  stats.overlay_missed_count = overlay_missed_count_;
  stats.overlay_gpu_success_count = overlay_gpu_success_count_;
  stats.overlay_gpu_failure_count = overlay_gpu_failure_count_;
  stats.overlay_cpu_fallback_count = overlay_cpu_fallback_count_;
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    stats.in_flight_metal_buffer_count = in_flight_draws_;
    stats.metal_buffer_exhaustion_count = metal_buffer_exhaustion_count_;
    stats.metal_command_completion_p95_us = metal_command_completion_p95_us_;
    stats.metal_command_failure_count = metal_command_failure_count_;
  }
  stats.async_metal_publish_active = 1;
  stats.video_source_update_count = video_source_update_count_;
  stats.viewport_composite_count = viewport_composite_count_;
  stats.source_frame_cache_hit_count = source_frame_cache_hit_count_;
  stats.source_frame_cache_miss_count = source_frame_cache_miss_count_;
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
  out->analysis_frame_index = last_draw_frame_info_.analysis_frame_index;
  out->frame_identity_mode = last_draw_frame_info_.frame_identity_mode;
  out->source_packet_index = last_draw_frame_info_.source_packet_index;
  out->source_packet_size = last_draw_frame_info_.source_packet_size;
  out->source_packet_pos = last_draw_frame_info_.source_packet_pos;
  out->source_packet_pts = last_draw_frame_info_.source_packet_pts;
  out->source_packet_dts = last_draw_frame_info_.source_packet_dts;
  out->color_range = last_draw_frame_info_.color_range;
  out->color_matrix = last_draw_frame_info_.color_matrix;
  out->color_transfer = last_draw_frame_info_.color_transfer;
  out->color_primaries = last_draw_frame_info_.color_primaries;
  out->target_pixel_buffer_address =
      last_draw_frame_info_.target_pixel_buffer_address;
  out->layout_revision = 0;
  return true;
}

bool MetalPresentationBackend::capture_front_buffer(std::vector<uint8_t>& bgra,
                                                    int& width,
                                                    int& height) {
  void* target = draw_target_pixel_buffer_;
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    if (target_ring_enabled_ && displayed_target_address_ != 0) {
      target = pointer_from_bits(displayed_target_address_);
    }
  }
  auto* pixel_buffer = static_cast<CVPixelBufferRef>(target);
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
                                                     size_t fill_rect_count,
                                                     size_t line_rect_count) {
  overlay_last_expected_ = expected;
  overlay_last_applied_ = applied;
  overlay_last_fill_rect_count_ = static_cast<uint64_t>(fill_rect_count);
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

void MetalPresentationBackend::invalidate_source_cache() {
  cached_package_valid_ = false;
  cached_package_source_signature_ = 0;
  last_source_signature_ = 0;
  cached_package_ = {};
}

bool MetalPresentationBackend::try_begin_async_draw(const char* source) {
  // The uploader can accept multiple per-frame resource slots, but this backend
  // still presents into one installed CVPixelBuffer target at a time. Keep
  // renderer-owned draws serialized until target ownership moves fully native.
  const uint64_t max_async_renderer_owned_draws =
      vp_macos::kMetalPresentConcurrencyPolicy.max_single_target_in_flight;
  std::lock_guard<std::mutex> lock(async_mutex_);
  if (in_flight_draws_ >= max_async_renderer_owned_draws) {
    ++metal_buffer_exhaustion_count_;
    set_last_error("renderer-owned Metal async draw deferred by backpressure");
    if (macos_profiler_enabled() &&
        (metal_buffer_exhaustion_count_ <= 8 ||
         (metal_buffer_exhaustion_count_ % 60) == 0)) {
      spdlog::info(
          "[MetalProfiler] async_backpressure source={} in_flight={} limit={} count={}",
          source ? source : "",
          in_flight_draws_,
          max_async_renderer_owned_draws,
          metal_buffer_exhaustion_count_);
    }
    return false;
  }
  ++in_flight_draws_;
  return true;
}

std::string MetalPresentationBackend::target_ring_state_summary_locked() const {
  std::ostringstream stream;
  stream << "ring=" << (target_ring_enabled_ ? "on" : "off")
         << " inFlight=" << in_flight_draws_
         << " displayed=0x" << std::hex << displayed_target_address_
         << " protected=0x" << protected_target_address_
         << std::dec << " states=";
  if (target_ring_.empty()) {
    stream << "empty";
    return stream.str();
  }
  for (const auto& slot : target_ring_) {
    char state = '?';
    switch (slot.state) {
      case TargetState::Available:
        state = 'a';
        break;
      case TargetState::InFlight:
        state = 'i';
        break;
      case TargetState::Completed:
        state = 'c';
        break;
      case TargetState::Displayed:
        state = 'd';
        break;
      case TargetState::Protected:
        state = 'p';
        break;
    }
    stream << state << ":0x" << std::hex << pointer_bits(slot.pixel_buffer) << std::dec
           << ",";
  }
  return stream.str();
}

void* MetalPresentationBackend::try_acquire_ring_draw_target(const char* source) {
  std::lock_guard<std::mutex> lock(async_mutex_);
  if (async_shutdown_) {
    set_last_error("renderer-owned Metal presentation backend is shutting down");
    return nullptr;
  }
  const uint64_t max_async_renderer_owned_draws =
      vp_macos::kMetalPresentConcurrencyPolicy.max_ring_in_flight;
  if (in_flight_draws_ >= max_async_renderer_owned_draws) {
    ++metal_buffer_exhaustion_count_;
    set_last_error("renderer-owned Metal async draw deferred by backpressure");
    return nullptr;
  }
  if (!target_ring_enabled_) {
    set_last_error("renderer-owned Metal presentation target ring is unavailable");
    return nullptr;
  }
  for (auto& slot : target_ring_) {
    if (slot.state != TargetState::Available || !slot.pixel_buffer) {
      continue;
    }
    slot.state = TargetState::InFlight;
    ++in_flight_draws_;
    return slot.pixel_buffer;
  }
  ++metal_buffer_exhaustion_count_;
  set_last_error("renderer-owned Metal presentation target ring is busy");
  if (macos_profiler_enabled() &&
      (metal_buffer_exhaustion_count_ <= 8 ||
       (metal_buffer_exhaustion_count_ % 60) == 0)) {
    spdlog::info(
        "[MetalProfiler] target_ring_backpressure source={} in_flight={} targets={} count={}",
        source ? source : "",
        in_flight_draws_,
        target_ring_.size(),
        metal_buffer_exhaustion_count_);
  }
  return nullptr;
}

void MetalPresentationBackend::complete_ring_draw_target(
    uint64_t target_pixel_buffer_address,
    bool success) {
  std::lock_guard<std::mutex> lock(async_mutex_);
  if (!target_ring_enabled_ || target_pixel_buffer_address == 0) {
    return;
  }
  for (auto& slot : target_ring_) {
    if (pointer_bits(slot.pixel_buffer) != target_pixel_buffer_address) {
      continue;
    }
    if (slot.state == TargetState::InFlight) {
      if (success) {
        slot.state = TargetState::Completed;
      } else if (target_pixel_buffer_address == displayed_target_address_) {
        slot.state = TargetState::Displayed;
      } else if (target_pixel_buffer_address == protected_target_address_) {
        slot.state = TargetState::Protected;
      } else {
        slot.state = TargetState::Available;
      }
    }
    return;
  }
}

void MetalPresentationBackend::finish_async_draw() {
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    if (in_flight_draws_ > 0) {
      --in_flight_draws_;
    }
  }
  async_cv_.notify_all();
}

bool MetalPresentationBackend::shutting_down_async() const {
  std::lock_guard<std::mutex> lock(async_mutex_);
  return async_shutdown_;
}

MetalPresentationBackend::DrawTicket MetalPresentationBackend::make_draw_ticket(
    uint64_t target_pixel_buffer_address,
    uint64_t source_signature,
    uint64_t overlay_generation,
    const char* path) {
  std::lock_guard<std::mutex> lock(async_mutex_);
  DrawTicket ticket;
  ticket.sequence = ++next_draw_ticket_sequence_;
  ticket.target_pixel_buffer_address = target_pixel_buffer_address;
  ticket.source_signature = source_signature;
  ticket.overlay_generation = overlay_generation;
  ticket.path = path ? path : "unknown";
  return ticket;
}

void MetalPresentationBackend::complete_async_draw_result(
    const DrawTicket& ticket,
    const VPMacOSNativeFrameInfo& frame_info,
    bool success,
    const char* error,
    int64_t total_us,
    bool overlay_expected,
    bool overlay_applied,
    bool gpu_attempted,
    bool gpu_succeeded,
    bool cpu_attempted,
    size_t fill_rect_count,
    size_t line_rect_count) {
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    if (total_us >= 0) {
      ++metal_command_completion_sample_count_;
      metal_command_completion_samples_us_.push_back(
          static_cast<uint64_t>(total_us));
      if (metal_command_completion_samples_us_.size() > 512) {
        metal_command_completion_samples_us_.erase(
            metal_command_completion_samples_us_.begin(),
            metal_command_completion_samples_us_.begin() +
                static_cast<std::ptrdiff_t>(
                    metal_command_completion_samples_us_.size() - 512));
      }
      if (metal_command_completion_sample_count_ <= 16 ||
          (metal_command_completion_sample_count_ % 16) == 0) {
        metal_command_completion_p95_us_ =
            percentile_95_us(metal_command_completion_samples_us_);
      }
    }
    if (!success) {
      ++metal_command_failure_count_;
    }
    if (success) {
      record_overlay_result(overlay_expected,
                            overlay_applied,
                            gpu_attempted,
                            gpu_succeeded,
                            cpu_attempted,
                            fill_rect_count,
                            line_rect_count);
      mark_draw_success(frame_info);
    } else {
      record_overlay_result(overlay_expected,
                            false,
                            gpu_attempted,
                            false,
                            cpu_attempted,
                            fill_rect_count,
                            line_rect_count);
      mark_draw_failure(error ? error : "renderer-owned Metal async draw failed");
    }
    if (target_ring_enabled_ && ticket.target_pixel_buffer_address != 0) {
      for (auto& slot : target_ring_) {
        if (pointer_bits(slot.pixel_buffer) != ticket.target_pixel_buffer_address) {
          continue;
        }
        if (slot.state == TargetState::InFlight) {
          if (success) {
            slot.state = TargetState::Completed;
          } else if (ticket.target_pixel_buffer_address == displayed_target_address_) {
            slot.state = TargetState::Displayed;
          } else if (ticket.target_pixel_buffer_address == protected_target_address_) {
            slot.state = TargetState::Protected;
          } else {
            slot.state = TargetState::Available;
          }
        }
        break;
      }
    }
    if (in_flight_draws_ > 0) {
      --in_flight_draws_;
    }
  }
  async_cv_.notify_all();
}

void metal_async_upload_completed(void* user_data,
                                  int ret,
                                  VPMacOSNativeFrameInfo frame_info,
                                  const char* error,
                                  int64_t gpu_wait_us,
                                  int64_t total_us) {
  std::unique_ptr<AsyncDrawContext> context(
      static_cast<AsyncDrawContext*>(user_data));
  if (!context || !context->backend) {
    return;
  }
  auto* backend = context->backend;
  const bool success = ret == 0;
  frame_info.target_pixel_buffer_address = context->ticket.target_pixel_buffer_address;
  auto overlay = context->overlay;
  if (overlay.expected) {
    overlay.gpu_attempted = true;
    overlay.gpu_succeeded = success;
    overlay.applied = success;
  }
  backend->complete_async_draw_result(context->ticket,
                                      frame_info,
                                      success,
                                      error,
                                      total_us,
                                      overlay.expected,
                                      overlay.applied,
                                      overlay.gpu_attempted,
                                      overlay.gpu_succeeded,
                                      overlay.cpu_attempted,
                                      overlay.fill_rect_count,
                                      overlay.line_rect_count);
  if (macos_profiler_enabled() &&
      (!success || gpu_wait_us >= 8000 || total_us >= 8000)) {
    spdlog::info(
        "[MetalProfiler] async_complete path={} success={} ret={} gpu_us={} total_us={} "
        "ticket={} target={} source={} overlay_generation={} bytes={} storage={} "
        "overlay_expected={} overlay_applied={} error={}",
        context->path,
        success,
        ret,
        gpu_wait_us,
        total_us,
        context->ticket.sequence,
        context->ticket.target_pixel_buffer_address,
        context->ticket.source_signature,
        context->ticket.overlay_generation,
        context->bytes,
        context->storage,
        overlay.expected,
        overlay.applied,
        error ? error : "");
  }
  if (context->hooks.async_draw_completed) {
    vr::PresentationBackendFrameInfo backend_frame_info;
    backend_frame_info.width = frame_info.width;
    backend_frame_info.height = frame_info.height;
    backend_frame_info.pts_us = frame_info.pts_us;
    backend_frame_info.dts_us = frame_info.dts_us;
    backend_frame_info.duration_us = frame_info.duration_us;
    backend_frame_info.analysis_frame_index = frame_info.analysis_frame_index;
    backend_frame_info.frame_identity_mode = frame_info.frame_identity_mode;
    backend_frame_info.source_packet_index = frame_info.source_packet_index;
    backend_frame_info.source_packet_size = frame_info.source_packet_size;
    backend_frame_info.source_packet_pos = frame_info.source_packet_pos;
    backend_frame_info.source_packet_pts = frame_info.source_packet_pts;
    backend_frame_info.source_packet_dts = frame_info.source_packet_dts;
    backend_frame_info.color_range = frame_info.color_range;
    backend_frame_info.color_matrix = frame_info.color_matrix;
    backend_frame_info.color_transfer = frame_info.color_transfer;
    backend_frame_info.color_primaries = frame_info.color_primaries;
    backend_frame_info.target_pixel_buffer_address =
        frame_info.target_pixel_buffer_address;
    backend_frame_info.layout_revision = 0;
    context->hooks.async_draw_completed(
        success,
        success ? "" : (error ? error : "renderer-owned Metal async draw failed"),
        static_cast<uint64_t>(std::max<int64_t>(0, total_us)),
        success ? &backend_frame_info : nullptr);
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
    const bool transient_backpressure =
        ret == -2 && error &&
        vr::is_transient_presentation_backpressure_error(error);
    if (transient_backpressure && draw_profiler_count_ % 120 != 0) {
      return;
    }
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
  const bool async_draw_requested = static_cast<bool>(hooks.async_draw_completed);
  bool use_target_ring = false;
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    use_target_ring = target_ring_enabled_;
  }
  void* target_pixel_buffer = draw_target_pixel_buffer_;
  if (async_draw_requested && use_target_ring) {
    target_pixel_buffer = try_acquire_ring_draw_target(hooks.draw_source);
    if (!target_pixel_buffer) {
      log_profiler("target-ring-backpressure", false, -2, 0, 0, 0,
                   last_error_.c_str());
      return false;
    }
  } else if (use_target_ring) {
    std::lock_guard<std::mutex> lock(async_mutex_);
    for (const auto& slot : target_ring_) {
      if (pointer_bits(slot.pixel_buffer) == displayed_target_address_) {
        target_pixel_buffer = slot.pixel_buffer;
        break;
      }
    }
    if (!target_pixel_buffer && !target_ring_.empty()) {
      target_pixel_buffer = target_ring_.front().pixel_buffer;
    }
  }
  auto release_acquired_target = [&]() {
    if (async_draw_requested && use_target_ring && target_pixel_buffer) {
      complete_ring_draw_target(pointer_bits(target_pixel_buffer), false);
      finish_async_draw();
      target_pixel_buffer = nullptr;
    }
  };
  if (!available() || !target_pixel_buffer ||
      draw_target_width_ <= 0 || draw_target_height_ <= 0) {
    const bool backend_available = available();
    std::string ring_state;
    {
      std::lock_guard<std::mutex> lock(async_mutex_);
      ring_state = target_ring_state_summary_locked();
    }
    release_acquired_target();
    mark_draw_failure("renderer-owned Metal presentation target is unavailable");
    spdlog::warn(
        "[MetalTarget] unavailable source={} available={} target=0x{:x} size={}x{} "
        "draw_target=0x{:x} {}",
        hooks.draw_source ? hooks.draw_source : "",
        backend_available,
        pointer_bits(target_pixel_buffer),
        draw_target_width_,
        draw_target_height_,
        pointer_bits(draw_target_pixel_buffer_),
        ring_state);
    log_profiler("none", false, -1, 0, 0, 0, last_error_.c_str());
    return false;
  }
  if (hooks.async_draw_completed && shutting_down_async()) {
    release_acquired_target();
    mark_draw_failure("renderer-owned Metal presentation backend is shutting down");
    log_profiler("shutdown", false, -1, 0, 0, 0, last_error_.c_str());
    return false;
  }

  const int32_t track_slots =
      std::clamp(draw_target_max_track_slots_,
                 1,
                 static_cast<int>(VPMacOSNativeMaxTracks));
  const bool is_viewport_composite =
      hooks.draw_source && std::strcmp(hooks.draw_source, "viewport_composite") == 0;
  if (is_viewport_composite) {
    ++viewport_composite_count_;
  }
  const uint64_t source_signature =
      source_frame_signature(snapshot, draw_target_width_, draw_target_height_, track_slots);
  const bool source_signature_hit =
      last_source_signature_ != 0 && last_source_signature_ == source_signature;
  if (source_signature_hit) {
    ++source_frame_cache_hit_count_;
  } else {
    ++source_frame_cache_miss_count_;
    ++video_source_update_count_;
    last_source_signature_ = source_signature;
  }
  const auto overlay_primitives_ptr = hooks.suppress_analysis_overlay
      ? empty_metal_overlay_primitives()
      : build_overlay_primitives_for_metal(
            snapshot, draw_target_width_, draw_target_height_);
  const auto& overlay_primitives = *overlay_primitives_ptr;
  const bool overlay_expected = overlay_primitives_expected(overlay_primitives);
  const auto overlay_set = overlay_primitive_set(overlay_primitives);
  const auto storage_extent =
      package_storage_extent(snapshot, draw_target_width_, draw_target_height_);
  const auto package_layout = vr::describe_presentation_package_layout(
      storage_extent.first, storage_extent.second, track_slots);
  if (package_layout.max_bytes == 0 ||
      package_layout.bgra_row_bytes >
          static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    release_acquired_target();
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
    VPMacOSNativeFrameInfoInit(&frame_info);
    char upload_error[256] = {};
    if (hooks.async_draw_completed) {
      auto* context = new AsyncDrawContext();
      context->backend = this;
      context->hooks = hooks;
      context->overlay = overlay_result_from_primitives(overlay_primitives);
      context->path = "cvpixelbuffer";
      context->storage = VPMacOSNativePresentPackageStorageCVPixelBuffer;
      context->target_pixel_buffer_address = pointer_bits(target_pixel_buffer);
      context->ticket = make_draw_ticket(context->target_pixel_buffer_address,
                                         source_signature,
                                         overlay_set.generation,
                                         context->path);
      if (!use_target_ring && !try_begin_async_draw(hooks.draw_source)) {
        delete context;
        log_profiler("cvpixelbuffer-backpressure",
                     false,
                     -2,
                     0,
                     0,
                     VPMacOSNativePresentPackageStorageCVPixelBuffer,
                     last_error_.c_str());
        return false;
      }
      const int ret =
          VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayoutAndOverlayAsync(
              uploader_,
              &cv_frame,
              overlay_expected ? &overlay_set : nullptr,
              target_pixel_buffer,
              draw_target_width_,
              draw_target_height_,
              &frame_info,
              upload_error,
              sizeof(upload_error),
              metal_async_upload_completed,
              context);
      if (hooks.record_frame_copy_us) {
        hooks.record_frame_copy_us(0);
      }
      if (ret == 0) {
        return true;
      }
      if (use_target_ring) {
        release_acquired_target();
      } else {
        finish_async_draw();
      }
      delete context;
      mark_draw_failure(upload_error[0] ? upload_error : error);
      log_profiler("cvpixelbuffer", false, ret, 0, 0,
                   VPMacOSNativePresentPackageStorageCVPixelBuffer,
                   last_error_.c_str());
      return false;
    }
    int ret = overlay_expected
        ? VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayoutAndOverlay(
              uploader_,
              &cv_frame,
              &overlay_set,
              target_pixel_buffer,
              draw_target_width_,
              draw_target_height_,
              &frame_info,
              upload_error,
              sizeof(upload_error))
        : VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayout(
              uploader_,
              &cv_frame,
              target_pixel_buffer,
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
                            overlay.fill_rect_count,
                            overlay.line_rect_count);
      annotate_frame_target(&frame_info, target_pixel_buffer);
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
          target_pixel_buffer,
          draw_target_width_,
          draw_target_height_,
          &frame_info,
          fallback_error,
          sizeof(fallback_error));
      if (ret == 0) {
        auto cpu_overlay = composite_overlay_after_upload(snapshot,
                                                          hooks,
                                                          uploader_,
                                                          target_pixel_buffer,
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
                              cpu_overlay.fill_rect_count,
                              cpu_overlay.line_rect_count);
        annotate_frame_target(&frame_info, target_pixel_buffer);
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
    VPMacOSNativeFrameInfoInit(&frame_info);
    char upload_error[256] = {};
    if (hooks.async_draw_completed) {
      auto* context = new AsyncDrawContext();
      context->backend = this;
      context->hooks = hooks;
      context->overlay = overlay_result_from_primitives(overlay_primitives);
      context->path = "cvpixelbuffer-set";
      context->storage = VPMacOSNativePresentPackageStorageCVPixelBuffer;
      context->target_pixel_buffer_address = pointer_bits(target_pixel_buffer);
      context->ticket = make_draw_ticket(context->target_pixel_buffer_address,
                                         source_signature,
                                         overlay_set.generation,
                                         context->path);
      if (!use_target_ring && !try_begin_async_draw(hooks.draw_source)) {
        delete context;
        log_profiler("cvpixelbuffer-set-backpressure",
                     false,
                     -2,
                     0,
                     0,
                     VPMacOSNativePresentPackageStorageCVPixelBuffer,
                     last_error_.c_str());
        return false;
      }
      const int ret =
          VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameSetWithLayoutAndOverlayAsync(
              uploader_,
              &cv_frame_set,
              overlay_expected ? &overlay_set : nullptr,
              target_pixel_buffer,
              draw_target_width_,
              draw_target_height_,
              &frame_info,
              upload_error,
              sizeof(upload_error),
              metal_async_upload_completed,
              context);
      if (hooks.record_frame_copy_us) {
        hooks.record_frame_copy_us(0);
      }
      if (ret == 0) {
        return true;
      }
      if (use_target_ring) {
        release_acquired_target();
      } else {
        finish_async_draw();
      }
      delete context;
      mark_draw_failure(upload_error[0] ? upload_error : error);
      log_profiler("cvpixelbuffer-set", false, ret, 0, 0,
                   VPMacOSNativePresentPackageStorageCVPixelBuffer,
                   last_error_.c_str());
      return false;
    }
    int ret = overlay_expected
        ? VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameSetWithLayoutAndOverlay(
              uploader_,
              &cv_frame_set,
              &overlay_set,
              target_pixel_buffer,
              draw_target_width_,
              draw_target_height_,
              &frame_info,
              upload_error,
              sizeof(upload_error))
        : VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameSetWithLayout(
              uploader_,
              &cv_frame_set,
              target_pixel_buffer,
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
                            overlay.fill_rect_count,
                            overlay.line_rect_count);
      annotate_frame_target(&frame_info, target_pixel_buffer);
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
          target_pixel_buffer,
          draw_target_width_,
          draw_target_height_,
          &frame_info,
          fallback_error,
          sizeof(fallback_error));
      if (ret == 0) {
        auto cpu_overlay = composite_overlay_after_upload(snapshot,
                                                          hooks,
                                                          uploader_,
                                                          target_pixel_buffer,
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
                              cpu_overlay.fill_rect_count,
                              cpu_overlay.line_rect_count);
        annotate_frame_target(&frame_info, target_pixel_buffer);
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

  const bool can_use_cached_package =
      cached_package_valid_ &&
      cached_package_source_signature_ == source_signature &&
      cached_package_.width == draw_target_width_ &&
      cached_package_.height == draw_target_height_ &&
      cached_package_.max_track_slots == track_slots &&
      cached_package_.storage != VPMacOSNativePresentPackageStorageUnavailable;
  bool package_from_cache = false;
  uint8_t* data = staging_buffer_.empty() ? nullptr : staging_buffer_.data();
  size_t data_size = staging_buffer_.size();
  if (can_use_cached_package) {
    package = cached_package_;
    update_cached_package_layout_decision(
        snapshot, draw_target_width_, draw_target_height_, &package);
    package_from_cache = true;
  } else {
    if (staging_buffer_.size() < package_layout.max_bytes) {
      staging_buffer_.assign(package_layout.max_bytes, 0);
      ++staging_allocation_count_;
      staging_max_bytes_ = std::max(staging_max_bytes_, staging_buffer_.size());
    } else {
      ++staging_reuse_count_;
    }
    data = staging_buffer_.data();
    data_size = staging_buffer_.size();
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
        release_acquired_target();
        mark_draw_failure(error);
        log_profiler("package-build", false, -1, 0, data_size, package.storage,
                     last_error_.c_str());
        return false;
      }
      package.storage = VPMacOSNativePresentPackageStorageBGRA;
    }
    cached_package_ = package;
    cached_package_source_signature_ = source_signature;
    cached_package_valid_ = true;
  }

  VPMacOSNativeFrameInfo frame_info = {};
  VPMacOSNativeFrameInfoInit(&frame_info);
  char upload_error[256] = {};
  const auto start = std::chrono::steady_clock::now();
  if (hooks.async_draw_completed) {
    auto* context = new AsyncDrawContext();
    context->backend = this;
    context->hooks = hooks;
    context->overlay = overlay_result_from_primitives(overlay_primitives);
    context->path =
        package.storage == VPMacOSNativePresentPackageStorageYUV
            ? (package_from_cache ? "package-yuv-cached-composite" : "package-yuv")
            : (package_from_cache ? "package-bgra-cached-composite" : "package-bgra");
    context->storage = package.storage;
    context->bytes = package.used_bytes;
    context->target_pixel_buffer_address = pointer_bits(target_pixel_buffer);
    context->ticket = make_draw_ticket(context->target_pixel_buffer_address,
                                       source_signature,
                                       overlay_set.generation,
                                       context->path);
    if (!use_target_ring && !try_begin_async_draw(hooks.draw_source)) {
      const char* failed_path = context->path;
      delete context;
      log_profiler(failed_path,
                   false,
                   -2,
                   0,
                   package.used_bytes,
                   package.storage,
                   last_error_.c_str());
      return false;
    }
    const int ret = VPMacOSMetalUploaderCopyPresentFramePackageWithLayoutAndOverlayAsync(
        uploader_,
        data,
        package.used_bytes,
        &package,
        overlay_expected ? &overlay_set : nullptr,
        target_pixel_buffer,
        draw_target_width_,
        draw_target_height_,
        &frame_info,
        upload_error,
        sizeof(upload_error),
        metal_async_upload_completed,
        context);
    const auto copy_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count();
    context->copy_us = copy_us;
    if (hooks.record_frame_copy_us) {
      hooks.record_frame_copy_us(static_cast<uint64_t>(copy_us));
    }
    if (ret == 0) {
      return true;
    }
    const char* failed_path = context->path;
    if (use_target_ring) {
      release_acquired_target();
    } else {
      finish_async_draw();
    }
    delete context;
    mark_draw_failure(upload_error[0] ? upload_error : error);
    log_profiler(failed_path,
                 false,
                 ret,
                 copy_us,
                 package.used_bytes,
                 package.storage,
                 last_error_.c_str());
    return false;
  }
  int ret = overlay_expected
      ? VPMacOSMetalUploaderCopyPresentFramePackageWithLayoutAndOverlay(
            uploader_,
            data,
            package.used_bytes,
            &package,
            &overlay_set,
            target_pixel_buffer,
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
            target_pixel_buffer,
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
                          overlay.fill_rect_count,
                          overlay.line_rect_count);
    annotate_frame_target(&frame_info, target_pixel_buffer);
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
          target_pixel_buffer,
          draw_target_width_,
          draw_target_height_,
          &frame_info,
          fallback_error,
          sizeof(fallback_error));
      if (ret == 0) {
        auto cpu_overlay = composite_overlay_after_upload(snapshot,
                                                          hooks,
                                                          uploader_,
                                                          target_pixel_buffer,
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
                              cpu_overlay.fill_rect_count,
                              cpu_overlay.line_rect_count);
        annotate_frame_target(&frame_info, target_pixel_buffer);
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
  const int32_t clamped_track_slots = std::clamp(
      max_track_slots, 1, static_cast<int32_t>(VPMacOSNativeMaxTracks));
  const bool source_cache_shape_changed =
      draw_target_width_ != width ||
      draw_target_height_ != height ||
      draw_target_max_track_slots_ != clamped_track_slots;
  draw_target_pixel_buffer_ = pixel_buffer;
  draw_target_width_ = width;
  draw_target_height_ = height;
  draw_target_max_track_slots_ = clamped_track_slots;
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    target_ring_.clear();
    target_ring_enabled_ = false;
    displayed_target_address_ = 0;
    protected_target_address_ = 0;
  }
  if (source_cache_shape_changed) {
    invalidate_source_cache();
  }
}

void MetalPresentationBackend::set_draw_target_ring(
    const void* const* pixel_buffers,
    size_t pixel_buffer_count,
    void* displayed_pixel_buffer,
    void* protected_pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots) {
  const int32_t clamped_track_slots = std::clamp(
      max_track_slots, 1, static_cast<int32_t>(VPMacOSNativeMaxTracks));
  const bool source_cache_shape_changed =
      draw_target_width_ != width ||
      draw_target_height_ != height ||
      draw_target_max_track_slots_ != clamped_track_slots;
  draw_target_pixel_buffer_ = nullptr;
  draw_target_width_ = width;
  draw_target_height_ = height;
  draw_target_max_track_slots_ = clamped_track_slots;
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    target_ring_.clear();
    target_ring_.reserve(pixel_buffer_count);
    displayed_target_address_ = pointer_bits(displayed_pixel_buffer);
    protected_target_address_ = pointer_bits(protected_pixel_buffer);
    for (size_t i = 0; i < pixel_buffer_count; ++i) {
      void* pixel_buffer = const_cast<void*>(pixel_buffers[i]);
      if (!pixel_buffer) {
        continue;
      }
      const uint64_t address = pointer_bits(pixel_buffer);
      TargetSlot slot;
      slot.pixel_buffer = pixel_buffer;
      if (address == displayed_target_address_) {
        slot.state = TargetState::Displayed;
      } else if (address == protected_target_address_) {
        slot.state = TargetState::Protected;
      } else {
        slot.state = TargetState::Available;
      }
      target_ring_.push_back(slot);
    }
    target_ring_enabled_ = target_ring_.size() >= 2;
    if (macos_profiler_enabled()) {
      spdlog::info(
          "[MetalTarget] install_ring targets={} displayed=0x{:x} protected=0x{:x} "
          "size={}x{} slots={} {}",
          target_ring_.size(),
          displayed_target_address_,
          protected_target_address_,
          draw_target_width_,
          draw_target_height_,
          draw_target_max_track_slots_,
          target_ring_state_summary_locked());
    }
  }
  if (source_cache_shape_changed) {
    invalidate_source_cache();
  }
}

void MetalPresentationBackend::clear_draw_target() {
  draw_target_pixel_buffer_ = nullptr;
  draw_target_width_ = 0;
  draw_target_height_ = 0;
  draw_target_max_track_slots_ = VPMacOSNativeMaxTracks;
  {
    std::lock_guard<std::mutex> lock(async_mutex_);
    target_ring_.clear();
    target_ring_enabled_ = false;
    displayed_target_address_ = 0;
    protected_target_address_ = 0;
  }
  spdlog::info("[MetalTarget] clear_draw_target");
  last_draw_frame_info_available_ = false;
  last_draw_frame_info_ = {};
  last_draw_succeeded_ = false;
  invalidate_source_cache();
  set_last_error("");
}

bool MetalPresentationBackend::contains_draw_target(void* pixel_buffer) const {
  if (!pixel_buffer) {
    return false;
  }
  std::lock_guard<std::mutex> lock(async_mutex_);
  if (!target_ring_enabled_) {
    return pixel_buffer == draw_target_pixel_buffer_;
  }
  return std::any_of(target_ring_.begin(), target_ring_.end(), [pixel_buffer](const TargetSlot& slot) {
    return slot.pixel_buffer == pixel_buffer;
  });
}

void MetalPresentationBackend::mark_displayed_target(void* pixel_buffer) {
  if (!pixel_buffer) {
    return;
  }
  std::lock_guard<std::mutex> lock(async_mutex_);
  if (!target_ring_enabled_) {
    return;
  }
  const uint64_t displayed = pointer_bits(pixel_buffer);
  displayed_target_address_ = displayed;
  for (auto& slot : target_ring_) {
    const uint64_t address = pointer_bits(slot.pixel_buffer);
    if (address == displayed) {
      slot.state = TargetState::Displayed;
    } else if (address == protected_target_address_) {
      slot.state = TargetState::Protected;
    } else if (slot.state == TargetState::Displayed ||
               slot.state == TargetState::Protected) {
      slot.state = TargetState::Available;
    }
  }
}

void MetalPresentationBackend::protect_target(void* pixel_buffer) {
  std::lock_guard<std::mutex> lock(async_mutex_);
  if (!target_ring_enabled_) {
    return;
  }
  protected_target_address_ = pointer_bits(pixel_buffer);
  for (auto& slot : target_ring_) {
    const uint64_t address = pointer_bits(slot.pixel_buffer);
    if (address == displayed_target_address_) {
      slot.state = TargetState::Displayed;
    } else if (pixel_buffer && address == protected_target_address_) {
      slot.state = TargetState::Protected;
    } else if (slot.state == TargetState::Protected) {
      slot.state = TargetState::Available;
    }
  }
}

void MetalPresentationBackend::release_target(void* pixel_buffer) {
  if (!pixel_buffer) {
    return;
  }
  std::lock_guard<std::mutex> lock(async_mutex_);
  if (!target_ring_enabled_) {
    return;
  }
  const uint64_t released = pointer_bits(pixel_buffer);
  for (auto& slot : target_ring_) {
    if (pointer_bits(slot.pixel_buffer) != released) {
      continue;
    }
    if (released == displayed_target_address_) {
      slot.state = TargetState::Displayed;
    } else if (released == protected_target_address_) {
      slot.state = TargetState::Protected;
    } else if (slot.state == TargetState::Completed) {
      slot.state = TargetState::Available;
    }
    return;
  }
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
