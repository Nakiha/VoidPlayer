#include "macos/wgpu/wgpu_metal_presentation_backend.h"

#include "macos/metal/metal_concurrency_policy.h"
#include "macos/presentation/presentation_package_builder.h"
#include "macos/metal/metal_texture_wrapping.h"
#include "macos/wgpu/wgpu_ffi_bridge.h"
#include "renderer/overlay/analysis_overlay_primitives.h"
#include "renderer/overlay/analysis_overlay_renderer.h"
#include "renderer/render/presentation_backend_factory.h"
#include "renderer/render/presentation_package.h"
#include "renderer/render/presentation_snapshot.h"

#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace vp_macos {
namespace {

uint64_t pointer_bits(const void* pointer) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer));
}

void* pointer_from_bits(uint64_t bits) {
  return reinterpret_cast<void*>(static_cast<uintptr_t>(bits));
}

std::string fixed_cstr(const char* value) {
  return value && value[0] != '\0' ? value : "unknown";
}

void hash_combine(uint64_t& seed, uint64_t value) {
  seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

bool wgpu_ffi_available() {
  return VPWgpuFfiVersion() >= VP_WGPU_FFI_ABI_VERSION;
}

bool wgpu_profiler_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("VOIDPLAYER_MACOS_WGPU_PROFILE");
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0 &&
           std::strcmp(value, "false") != 0;
  }();
  return enabled;
}

void log_wgpu_decision(const char* route,
                       const VPMacOSNativePresentDecisionInfo& decision,
                       const float* viewport_rect,
                       int32_t storage,
                       int32_t output_format,
                       int32_t output_color_mode,
                       void* destination_texture) {
  if (!wgpu_profiler_enabled()) {
    return;
  }
  spdlog::info(
      "[WgpuMetalProfile] decision route={} dst=0x{:x} storage={} "
      "output_format={} output_color_mode={} mode={} tracks={} split={:.4f} "
      "present=[{},{},{},{}] order=[{},{},{},{}] "
      "src_w=[{},{},{},{}] src_h=[{},{},{},{}] "
      "transfer=[{},{},{},{}] primaries=[{},{},{},{}] "
      "viewport=({:.1f},{:.1f},{:.1f},{:.1f}) "
      "display_x=[{:.4f},{:.4f},{:.4f},{:.4f}] "
      "display_y=[{:.4f},{:.4f},{:.4f},{:.4f}] "
      "inv_x=[{:.4f},{:.4f},{:.4f},{:.4f}] "
      "inv_y=[{:.4f},{:.4f},{:.4f},{:.4f}] "
      "view_x=[{:.4f},{:.4f},{:.4f},{:.4f}] "
      "view_y=[{:.4f},{:.4f},{:.4f},{:.4f}]",
      route ? route : "",
      pointer_bits(destination_texture),
      storage,
      output_format,
      output_color_mode,
      decision.mode,
      decision.track_count,
      decision.split_pos,
      decision.frames[0].present,
      decision.frames[1].present,
      decision.frames[2].present,
      decision.frames[3].present,
      decision.order[0],
      decision.order[1],
      decision.order[2],
      decision.order[3],
      decision.source_width[0],
      decision.source_width[1],
      decision.source_width[2],
      decision.source_width[3],
      decision.source_height[0],
      decision.source_height[1],
      decision.source_height[2],
      decision.source_height[3],
      decision.color_transfer[0],
      decision.color_transfer[1],
      decision.color_transfer[2],
      decision.color_transfer[3],
      decision.color_primaries[0],
      decision.color_primaries[1],
      decision.color_primaries[2],
      decision.color_primaries[3],
      viewport_rect ? viewport_rect[0] : 0.0f,
      viewport_rect ? viewport_rect[1] : 0.0f,
      viewport_rect ? viewport_rect[2] : 0.0f,
      viewport_rect ? viewport_rect[3] : 0.0f,
      decision.display_offset_x[0],
      decision.display_offset_x[1],
      decision.display_offset_x[2],
      decision.display_offset_x[3],
      decision.display_offset_y[0],
      decision.display_offset_y[1],
      decision.display_offset_y[2],
      decision.display_offset_y[3],
      decision.inv_display_size_x[0],
      decision.inv_display_size_x[1],
      decision.inv_display_size_x[2],
      decision.inv_display_size_x[3],
      decision.inv_display_size_y[0],
      decision.inv_display_size_y[1],
      decision.inv_display_size_y[2],
      decision.inv_display_size_y[3],
      decision.view_offset_uv_x[0],
      decision.view_offset_uv_x[1],
      decision.view_offset_uv_x[2],
      decision.view_offset_uv_x[3],
      decision.view_offset_uv_y[0],
      decision.view_offset_uv_y[1],
      decision.view_offset_uv_y[2],
      decision.view_offset_uv_y[3]);
}

void log_cv_pixel_buffer_sample(CVPixelBufferRef pixel_buffer,
                                bool is_p010,
                                size_t slot,
                                const VPMacOSNativePresentDecisionInfo& decision) {
  if (!wgpu_profiler_enabled() || !pixel_buffer) {
    return;
  }
  static int sample_log_count = 0;
  if (sample_log_count >= 12) {
    return;
  }
  ++sample_log_count;
  if (CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly) !=
      kCVReturnSuccess) {
    spdlog::info("[WgpuMetalProfile] cv_sample slot={} lock_failed=true", slot);
    return;
  }
  const int width = static_cast<int>(CVPixelBufferGetWidth(pixel_buffer));
  const int height = static_cast<int>(CVPixelBufferGetHeight(pixel_buffer));
  const int y_stride =
      static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 0));
  const int uv_stride =
      static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(pixel_buffer, 1));
  const int x = std::clamp(width / 2, 0, std::max(0, width - 1));
  const int y = std::clamp(height / 2, 0, std::max(0, height - 1));
  const int uv_x = x / 2;
  const int uv_y = y / 2;
  const auto* y_base = static_cast<const uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 0));
  const auto* uv_base = static_cast<const uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(pixel_buffer, 1));
  uint32_t y_raw = 0;
  uint32_t u_raw = 0;
  uint32_t v_raw = 0;
  if (y_base && uv_base && is_p010) {
    const auto* y_ptr =
        reinterpret_cast<const uint16_t*>(y_base + y * y_stride) + x;
    const auto* uv_ptr =
        reinterpret_cast<const uint16_t*>(uv_base + uv_y * uv_stride) +
        uv_x * 2;
    y_raw = y_ptr[0];
    u_raw = uv_ptr[0];
    v_raw = uv_ptr[1];
  } else if (y_base && uv_base) {
    y_raw = y_base[y * y_stride + x];
    u_raw = uv_base[uv_y * uv_stride + uv_x * 2];
    v_raw = uv_base[uv_y * uv_stride + uv_x * 2 + 1];
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  const uint32_t y_10 = is_p010 ? (y_raw >> 6) : y_raw;
  const uint32_t u_10 = is_p010 ? (u_raw >> 6) : u_raw;
  const uint32_t v_10 = is_p010 ? (v_raw >> 6) : v_raw;
  spdlog::info(
      "[WgpuMetalProfile] cv_sample slot={} p010={} size={}x{} stride={}/{} "
      "xy={},{} raw_yuv=[{},{},{}] shifted_yuv=[{},{},{}] "
      "range={} matrix={} transfer={} primaries={}",
      slot,
      is_p010,
      width,
      height,
      y_stride,
      uv_stride,
      x,
      y,
      y_raw,
      u_raw,
      v_raw,
      y_10,
      u_10,
      v_10,
      decision.color_range[slot],
      decision.color_matrix[slot],
      decision.color_transfer[slot],
      decision.color_primaries[slot]);
}

bool metal_texture_matches_device(id<MTLTexture> texture, void* metal_device) {
  if (!texture || !metal_device) {
    return false;
  }
  return [texture device] == (__bridge id<MTLDevice>)metal_device;
}

bool validate_target_texture_device(void* texture_cache,
                                    void* pixel_buffer,
                                    MTLPixelFormat metal_pixel_format,
                                    int32_t width,
                                    int32_t height,
                                    void* metal_device,
                                    std::string& error) {
  if (!texture_cache || !pixel_buffer || !metal_device || width <= 0 || height <= 0) {
    error = "wgpu-metal target texture validation arguments are invalid";
    return false;
  }
  CVMetalTextureRef texture_ref = nullptr;
  const CVReturn status = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      static_cast<CVMetalTextureCacheRef>(texture_cache),
      static_cast<CVPixelBufferRef>(pixel_buffer),
      nullptr,
      metal_pixel_format,
      width,
      height,
      0,
      &texture_ref);
  if (status != kCVReturnSuccess || !texture_ref) {
    if (texture_ref) {
      CFRelease(texture_ref);
    }
    error = "wgpu-metal failed to validate target CVPixelBuffer texture";
    return false;
  }
  id<MTLTexture> texture = CVMetalTextureGetTexture(texture_ref);
  const bool matches = metal_texture_matches_device(texture, metal_device);
  CFRelease(texture_ref);
  if (!matches) {
    error = "wgpu-metal target texture device does not match wgpu Metal device";
    return false;
  }
  return true;
}

uint64_t elapsed_us_since(std::chrono::steady_clock::time_point start) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
}

uint64_t percentile_95_us(std::vector<uint64_t> samples) {
  if (samples.empty()) {
    return 0;
  }
  std::sort(samples.begin(), samples.end());
  const size_t index =
      std::min(samples.size() - 1, ((samples.size() * 95u) + 99u) / 100u - 1u);
  return samples[index];
}

CVPixelBufferRef as_pixel_buffer(void* pixel_buffer) {
  return static_cast<CVPixelBufferRef>(pixel_buffer);
}

vr::PresentationBackendFrameInfo frame_info_from_decision(
    const VPMacOSNativePresentDecisionInfo& decision,
    void* target_pixel_buffer) {
  vr::PresentationBackendFrameInfo result;
  for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    const auto& frame = decision.frames[slot];
    if (!frame.present) {
      continue;
    }
    result.width = frame.width;
    result.height = frame.height;
    result.pts_us = frame.pts_us;
    result.dts_us = frame.dts_us;
    result.duration_us = frame.duration_us;
    result.analysis_frame_index = frame.analysis_frame_index;
    result.frame_identity_mode = frame.frame_identity_mode;
    result.source_packet_index = frame.source_packet_index;
    result.source_packet_size = frame.source_packet_size;
    result.source_packet_pos = frame.source_packet_pos;
    result.source_packet_pts = frame.source_packet_pts;
    result.source_packet_dts = frame.source_packet_dts;
    result.color_range = frame.color_range;
    result.color_matrix = frame.color_matrix;
    result.color_transfer = frame.color_transfer;
    result.color_primaries = frame.color_primaries;
    break;
  }
  result.target_pixel_buffer_address = pointer_bits(target_pixel_buffer);
  return result;
}

vr::PresentationBackendFrameInfo frame_info_from_package(
    const VPMacOSNativePresentFramePackageInfo& package,
    void* target_pixel_buffer) {
  return frame_info_from_decision(package.decision, target_pixel_buffer);
}

struct SnapshotStorageMix {
  bool any_frame = false;
  bool any_cv_pixel_buffer = false;
  bool any_non_cv_pixel_buffer = false;
};

struct ExternalFlutterSurfaceSnapshot {
  void* texture_ref = nullptr;
  int32_t width = 0;
  int32_t height = 0;
  uint64_t frame_generation = 0;

  ExternalFlutterSurfaceSnapshot() = default;
  ExternalFlutterSurfaceSnapshot(const ExternalFlutterSurfaceSnapshot&) = delete;
  ExternalFlutterSurfaceSnapshot& operator=(
      const ExternalFlutterSurfaceSnapshot&) = delete;
  ~ExternalFlutterSurfaceSnapshot() {
    if (texture_ref) {
      CFRelease(texture_ref);
    }
  }

  bool valid() const {
    return texture_ref && width > 0 && height > 0;
  }
};

SnapshotStorageMix snapshot_storage_mix(const vr::RendererDrawSnapshot& snapshot) {
  SnapshotStorageMix mix;
  for (const auto& frame : snapshot.decision.frames) {
    if (!frame.has_value()) {
      continue;
    }
    mix.any_frame = true;
    if (frame->cv_pixel_buffer_storage()) {
      mix.any_cv_pixel_buffer = true;
    } else {
      mix.any_non_cv_pixel_buffer = true;
    }
  }
  return mix;
}

size_t snapshot_bgra_fallback_package_bytes(
    const vr::RendererDrawSnapshot& snapshot) {
  size_t total = 0;
  for (const auto& frame : snapshot.decision.frames) {
    if (!frame.has_value() ||
        !vr::frame_storage_has_cpu_pixels(frame->storage_kind()) ||
        frame->width <= 0 || frame->height <= 0 ||
        frame->width > std::numeric_limits<int32_t>::max() / 4) {
      continue;
    }
    const size_t row_bytes = static_cast<size_t>(frame->width) * 4u;
    if (static_cast<size_t>(frame->height) >
            std::numeric_limits<size_t>::max() / row_bytes ||
        total > std::numeric_limits<size_t>::max() -
                    row_bytes * static_cast<size_t>(frame->height)) {
      return 0;
    }
    total += row_bytes * static_cast<size_t>(frame->height);
  }
  return total;
}

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

struct WgpuOverlayPrimitiveBuildResult {
  std::vector<VPMacOSNativeOverlayGpuRect> fill_rects;
  std::vector<VPMacOSNativeOverlayGpuRect> line_rects;
  std::vector<VPMacOSNativeOverlayGpuRect> motion_lines;
  uint64_t generation = 0;
};

bool overlay_primitives_expected(const WgpuOverlayPrimitiveBuildResult& overlay) {
  return !overlay.fill_rects.empty() || !overlay.line_rects.empty() ||
         !overlay.motion_lines.empty();
}

struct WgpuOverlayPrimitiveCacheKey {
  const void* package = nullptr;
  uint64_t generation = 0;

  bool operator==(const WgpuOverlayPrimitiveCacheKey& other) const {
    return package == other.package && generation == other.generation;
  }
};

struct WgpuOverlayPrimitiveCacheEntry {
  WgpuOverlayPrimitiveCacheKey key;
  std::shared_ptr<const WgpuOverlayPrimitiveBuildResult> result;
  uint64_t last_used = 0;
};

struct WgpuOutputTargetDescriptor {
  MTLPixelFormat metal_pixel_format = MTLPixelFormatInvalid;
  int32_t ffi_output_format = 0;
  int32_t ffi_output_color_mode = 0;
  const char* render_target_format = "unknown";
  const char* render_color_space = "unknown";
  bool render_supported = false;
};

bool resolve_metal_texture_target_descriptor(
    id<MTLTexture> texture,
    int32_t expected_width,
    int32_t expected_height,
    WgpuOutputTargetDescriptor& descriptor,
    std::string& error) {
  if (!texture) {
    error = "wgpu-metal Metal texture target is unavailable";
    return false;
  }
  if (static_cast<int32_t>(texture.width) != expected_width ||
      static_cast<int32_t>(texture.height) != expected_height) {
    error =
        "wgpu-metal Metal texture target dimensions do not match the presentation surface";
    return false;
  }
  switch (texture.pixelFormat) {
    case MTLPixelFormatBGRA8Unorm:
      descriptor.metal_pixel_format = MTLPixelFormatBGRA8Unorm;
      descriptor.ffi_output_format = VP_WGPU_METAL_OUTPUT_FORMAT_BGRA8_UNORM;
      descriptor.ffi_output_color_mode = VP_WGPU_METAL_OUTPUT_COLOR_MODE_SDR;
      descriptor.render_target_format = "BGRA8";
      descriptor.render_color_space = "wgpu-metal-sdr";
      descriptor.render_supported = true;
      return true;
    case MTLPixelFormatRGBA16Float:
      descriptor.metal_pixel_format = MTLPixelFormatRGBA16Float;
      descriptor.ffi_output_format = VP_WGPU_METAL_OUTPUT_FORMAT_RGBA16_FLOAT;
      descriptor.ffi_output_color_mode =
          VP_WGPU_METAL_OUTPUT_COLOR_MODE_MACOS_EDR;
      descriptor.render_target_format = "RGBA16Float";
      descriptor.render_color_space = "wgpu-metal-edr";
      descriptor.render_supported = true;
      return true;
    default:
      error = "wgpu-metal Metal texture target format is unsupported";
      return false;
  }
}

bool resolve_output_format_descriptor(void* target,
                                      WgpuOutputTargetDescriptor& descriptor,
                                      std::string& error) {
  auto* pixel_buffer = as_pixel_buffer(target);
  if (!pixel_buffer) {
    error = "wgpu-metal presentation target is unavailable";
    return false;
  }
  switch (CVPixelBufferGetPixelFormatType(pixel_buffer)) {
    case kCVPixelFormatType_32BGRA:
      descriptor.metal_pixel_format = MTLPixelFormatBGRA8Unorm;
      descriptor.ffi_output_format = VP_WGPU_METAL_OUTPUT_FORMAT_BGRA8_UNORM;
      descriptor.ffi_output_color_mode = VP_WGPU_METAL_OUTPUT_COLOR_MODE_SDR;
      descriptor.render_target_format = "BGRA8";
      descriptor.render_color_space = "wgpu-metal-sdr";
      descriptor.render_supported = true;
      return true;
    case kCVPixelFormatType_64RGBAHalf:
      descriptor.metal_pixel_format = MTLPixelFormatRGBA16Float;
      descriptor.ffi_output_format = VP_WGPU_METAL_OUTPUT_FORMAT_RGBA16_FLOAT;
      descriptor.ffi_output_color_mode =
          VP_WGPU_METAL_OUTPUT_COLOR_MODE_MACOS_EDR;
      descriptor.render_target_format = "RGBA16Float";
      descriptor.render_color_space = "wgpu-metal-edr";
      descriptor.render_supported = true;
      return true;
    default:
      error = "wgpu-metal target pixel buffer format is unsupported";
      return false;
  }
}

bool resolve_output_target_descriptor(void* target,
                                      int32_t expected_width,
                                      int32_t expected_height,
                                      WgpuOutputTargetDescriptor& descriptor,
                                      std::string& error) {
  if (!resolve_output_format_descriptor(target, descriptor, error)) {
    return false;
  }
  auto* pixel_buffer = as_pixel_buffer(target);
  const int32_t actual_width =
      static_cast<int32_t>(CVPixelBufferGetWidth(pixel_buffer));
  const int32_t actual_height =
      static_cast<int32_t>(CVPixelBufferGetHeight(pixel_buffer));
  if (actual_width != expected_width || actual_height != expected_height) {
    error = "wgpu-metal target pixel buffer dimensions do not match the presentation surface";
    return false;
  }
  return true;
}

uint64_t source_frame_signature(const vr::RendererDrawSnapshot& snapshot,
                                int32_t output_format,
                                int32_t output_color_mode) {
  uint64_t hash = 1469598103934665603ull;
  hash_combine(hash, snapshot.decision.should_present ? 1u : 0u);
  hash_combine(hash, static_cast<uint64_t>(output_format));
  hash_combine(hash, static_cast<uint64_t>(output_color_mode));
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
    hash_combine(hash, static_cast<uint64_t>(frame->analysis_frame_index));
    hash_combine(hash, static_cast<uint64_t>(frame->frame_identity_mode));
    hash_combine(hash, static_cast<uint64_t>(frame->source_packet_index));
    hash_combine(hash, static_cast<uint64_t>(frame->source_packet_pos));
    hash_combine(hash, static_cast<uint64_t>(frame->source_packet_pts));
    hash_combine(hash, static_cast<uint64_t>(frame->source_packet_dts));
    hash_combine(hash, static_cast<uint64_t>(frame->width));
    hash_combine(hash, static_cast<uint64_t>(frame->height));
    hash_combine(hash, static_cast<uint64_t>(frame->storage_kind()));
    const int color_range = frame->color.range != vr::VIDEO_COLOR_RANGE_UNKNOWN
        ? frame->color.range
        : vr::VIDEO_COLOR_RANGE_LIMITED;
    const int color_matrix = frame->color.matrix != vr::VIDEO_COLOR_MATRIX_UNKNOWN
        ? frame->color.matrix
        : vr::default_presentation_color_matrix_for_size(frame->width, frame->height);
    const int color_transfer =
        frame->color.transfer != vr::VIDEO_COLOR_TRANSFER_UNKNOWN
        ? frame->color.transfer
        : vr::VIDEO_COLOR_TRANSFER_SDR;
    const int color_primaries =
        frame->color.primaries != vr::VIDEO_COLOR_PRIMARIES_UNKNOWN
        ? frame->color.primaries
        : vr::default_presentation_color_primaries_for_matrix(color_matrix);
    hash_combine(hash, static_cast<uint64_t>(color_range));
    hash_combine(hash, static_cast<uint64_t>(color_matrix));
    hash_combine(hash, static_cast<uint64_t>(color_transfer));
    hash_combine(hash, static_cast<uint64_t>(color_primaries));
    if (const auto* rgba = frame->cpu_rgba_storage()) {
      hash_combine(hash, static_cast<uint64_t>(rgba->stride));
    } else if (const auto* nv12 = frame->cpu_nv12_storage()) {
      hash_combine(hash, static_cast<uint64_t>(nv12->y_stride));
      hash_combine(hash, static_cast<uint64_t>(nv12->uv_stride));
      hash_combine(hash, nv12->is_p010 ? 1u : 0u);
      hash_combine(hash, static_cast<uint64_t>(nv12->coded_width));
      hash_combine(hash, static_cast<uint64_t>(nv12->coded_height));
    } else if (const auto* planar = frame->cpu_planar_yuv_storage()) {
      for (int plane = 0; plane < 3; ++plane) {
        hash_combine(hash, static_cast<uint64_t>(planar->strides[plane]));
        hash_combine(hash, static_cast<uint64_t>(planar->plane_widths[plane]));
        hash_combine(hash, static_cast<uint64_t>(planar->plane_heights[plane]));
      }
      hash_combine(hash, static_cast<uint64_t>(planar->bytes_per_sample));
    } else if (const auto* cv_pixel = frame->cv_pixel_buffer_storage()) {
      hash_combine(hash, static_cast<uint64_t>(cv_pixel->pixel_format));
      hash_combine(hash, static_cast<uint64_t>(cv_pixel->plane_count));
      hash_combine(hash, cv_pixel->is_p010 ? 1u : 0u);
      hash_combine(hash, static_cast<uint64_t>(cv_pixel->coded_width));
      hash_combine(hash, static_cast<uint64_t>(cv_pixel->coded_height));
    }
  }
  return hash;
}

constexpr size_t kWgpuOverlayPrimitiveCacheLimit = 24;

std::mutex& wgpu_overlay_primitive_cache_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::vector<WgpuOverlayPrimitiveCacheEntry>&
wgpu_overlay_primitive_cache_entries() {
  static std::vector<WgpuOverlayPrimitiveCacheEntry> entries;
  return entries;
}

thread_local bool g_in_wgpu_async_completion_callback = false;

uint64_t& wgpu_overlay_primitive_cache_clock() {
  static uint64_t clock = 0;
  return clock;
}

std::shared_ptr<const WgpuOverlayPrimitiveBuildResult>
lookup_wgpu_overlay_primitives(WgpuOverlayPrimitiveCacheKey key) {
  std::lock_guard<std::mutex> lock(wgpu_overlay_primitive_cache_mutex());
  auto& clock = wgpu_overlay_primitive_cache_clock();
  const uint64_t use_token = ++clock;
  for (auto& entry : wgpu_overlay_primitive_cache_entries()) {
    if (entry.key == key) {
      entry.last_used = use_token;
      return entry.result;
    }
  }
  return nullptr;
}

void store_wgpu_overlay_primitives(
    WgpuOverlayPrimitiveCacheKey key,
    std::shared_ptr<const WgpuOverlayPrimitiveBuildResult> result) {
  if (!result) {
    return;
  }
  std::lock_guard<std::mutex> lock(wgpu_overlay_primitive_cache_mutex());
  auto& clock = wgpu_overlay_primitive_cache_clock();
  auto& entries = wgpu_overlay_primitive_cache_entries();
  const uint64_t use_token = ++clock;
  for (auto& entry : entries) {
    if (entry.key == key) {
      entry.result = std::move(result);
      entry.last_used = use_token;
      return;
    }
  }
  if (entries.size() >= kWgpuOverlayPrimitiveCacheLimit) {
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
      WgpuOverlayPrimitiveCacheEntry{key, std::move(result), use_token});
}

void apply_source_projection_to_macos_decision(
    const vr::PresentationSourceProjection& projection,
    VPMacOSNativePresentDecisionInfo& decision) {
  if (!projection.enabled) {
    return;
  }
  decision.mode = projection.mode;
  decision.split_pos = projection.split_pos;
  decision.track_count = std::clamp(
      projection.active_track_count, 1, static_cast<int>(VPMacOSNativeMaxTracks));
  for (size_t i = 0; i < VPMacOSNativeMaxTracks; ++i) {
    const int source_slot = projection.source_order[i];
    decision.order[i] =
        source_slot >= 0 && source_slot < static_cast<int>(VPMacOSNativeMaxTracks)
            ? source_slot
            : -1;
    decision.display_offset_x[i] = projection.display_offset_x[i];
    decision.display_offset_y[i] = projection.display_offset_y[i];
    decision.inv_display_size_x[i] = projection.inv_display_size_x[i];
    decision.inv_display_size_y[i] = projection.inv_display_size_y[i];
    decision.view_offset_uv_x[i] = projection.view_offset_uv_x[i];
    decision.view_offset_uv_y[i] = projection.view_offset_uv_y[i];
  }
}

WgpuOverlayPrimitiveBuildResult build_overlay_primitives_for_wgpu(
    const vr::RendererDrawSnapshot& snapshot,
    const vr::PresentationBackendDrawHooks& hooks) {
  WgpuOverlayPrimitiveBuildResult result;
  if (hooks.suppress_analysis_overlay || !hooks.build_overlay_primitives) {
    return result;
  }
  const auto package = hooks.build_overlay_primitives(snapshot);
  if (!package || package->empty()) {
    return result;
  }
  const WgpuOverlayPrimitiveCacheKey cache_key{
      package.get(),
      package->cache_generation,
  };
  if (const auto cached = lookup_wgpu_overlay_primitives(cache_key)) {
    return *cached;
  }

  auto cached_result = std::make_shared<WgpuOverlayPrimitiveBuildResult>();
  cached_result->generation = package->cache_generation;
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
      cached_result->fill_rects.push_back(rect);
    }
    for (const auto& primitive : track.outline_rects) {
      VPMacOSNativeOverlayGpuRect rect = {};
      rect.rect_uv0 = vr::pack_overlay_uv16(
          primitive.x0, track.video_width, primitive.y0, track.video_height);
      rect.rect_uv1 = vr::pack_overlay_uv16(
          primitive.x1, track.video_width, primitive.y1, track.video_height);
      rect.track_idx = pack_overlay_track_payload(track.slot, track.line_alpha);
      cached_result->line_rects.push_back(rect);
    }
    for (const auto& line : track.motion_lines) {
      VPMacOSNativeOverlayGpuRect rect = {};
      rect.rect_uv0 = vr::pack_overlay_uv16(
          line.x0, track.video_width, line.y0, track.video_height);
      rect.rect_uv1 = vr::pack_overlay_uv16(
          line.x1, track.video_width, line.y1, track.video_height);
      rect.color_bgra = pack_overlay_bgra(line.color);
      rect.track_idx = pack_overlay_track_payload(track.slot, track.line_alpha);
      cached_result->motion_lines.push_back(rect);
    }
  }
  store_wgpu_overlay_primitives(cache_key, cached_result);
  return *cached_result;
}

}  // namespace

WgpuMetalPresentationBackend::WgpuMetalPresentationBackend()
    : async_state_(std::make_shared<AsyncState>()) {
  async_state_->backend = this;
}

WgpuMetalPresentationBackend::AsyncDrawPending::~AsyncDrawPending() {
  if (destination_texture_ref) {
    CFRelease(destination_texture_ref);
    destination_texture_ref = nullptr;
  }
  for (void*& pixel_buffer_ref : source_pixel_buffer_refs) {
    if (pixel_buffer_ref) {
      CFRelease(pixel_buffer_ref);
      pixel_buffer_ref = nullptr;
    }
  }
  for (void*& texture_ref : source_y_texture_refs) {
    if (texture_ref) {
      CFRelease(texture_ref);
      texture_ref = nullptr;
    }
  }
  for (void*& texture_ref : source_uv_texture_refs) {
    if (texture_ref) {
      CFRelease(texture_ref);
      texture_ref = nullptr;
    }
  }
}

void wgpu_async_draw_completed(void* user_data, int32_t result) {
  std::unique_ptr<WgpuMetalPresentationBackend::AsyncDrawPending> pending(
      static_cast<WgpuMetalPresentationBackend::AsyncDrawPending*>(user_data));
  if (!pending || !pending->state) {
    return;
  }
  auto state = pending->state;
  WgpuMetalPresentationBackend* backend = nullptr;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->shutdown) {
      backend = state->backend;
      if (backend) {
        ++state->active_callbacks;
      }
    }
  }
  if (!backend) {
    return;
  }
  g_in_wgpu_async_completion_callback = true;
  backend->complete_async_draw(std::move(pending), result == 0);
  g_in_wgpu_async_completion_callback = false;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->active_callbacks > 0) {
    --state->active_callbacks;
  }
  state->cv.notify_all();
}

WgpuMetalPresentationBackend::~WgpuMetalPresentationBackend() {
  shutdown();
}

const char* WgpuMetalPresentationBackend::last_error() const {
  thread_local std::string error_copy;
  std::lock_guard<std::mutex> lock(mutex_);
  error_copy = last_error_;
  return error_copy.c_str();
}

bool WgpuMetalPresentationBackend::initialize(const vr::PresentationBackendConfig& config) {
  shutdown();
  async_state_ = std::make_shared<AsyncState>();
  async_state_->backend = this;
  headless_ = config.headless;
  width_ = config.width;
  height_ = config.height;
  draw_target_max_track_slots_ = std::max(1, config.max_track_slots);

  if (!wgpu_ffi_available()) {
    mark_draw_failure("wgpu-metal Rust FFI is not linked");
    return false;
  }
  char ffi_error[256] = {};
  wgpu_renderer_ = VPWgpuMetalRendererCreate(ffi_error, sizeof(ffi_error));
  if (!wgpu_renderer_) {
    mark_draw_failure(ffi_error[0] ? ffi_error : "wgpu-metal renderer create failed");
    shutdown();
    return false;
  }
  void* wgpu_metal_device = VPWgpuMetalRendererMetalDevice(wgpu_renderer_);
  if (!wgpu_metal_device) {
    mark_draw_failure("wgpu-metal failed to expose wgpu Metal device");
    shutdown();
    return false;
  }
  id<MTLDevice> device = (__bridge id<MTLDevice>)wgpu_metal_device;
  metal_device_ = (__bridge_retained void*)device;
  CVMetalTextureCacheRef cache = nullptr;
  if (CVMetalTextureCacheCreate(kCFAllocatorDefault,
                                nullptr,
                                (__bridge id<MTLDevice>)metal_device_,
                                nullptr,
                                &cache) != kCVReturnSuccess ||
      !cache) {
    mark_draw_failure("wgpu-metal failed to create CVMetalTextureCache");
    shutdown();
    return false;
  }
  texture_cache_ = cache;
  if (config.output && !update_headless_output(config.output,
                                               config.width,
                                               config.height,
                                               config.max_track_slots)) {
    shutdown();
    return false;
  }
  VPWgpuMetalRendererInfo renderer_info = {};
  if (VPWgpuMetalRendererGetInfo(wgpu_renderer_, &renderer_info) == 0) {
    wgpu_adapter_description_ = fixed_cstr(renderer_info.adapter_description);
    wgpu_driver_type_ = fixed_cstr(renderer_info.driver_type);
    wgpu_backend_name_ = fixed_cstr(renderer_info.backend);
    wgpu_device_type_ = fixed_cstr(renderer_info.device_type);
    wgpu_adapter_vendor_id_ = renderer_info.vendor_id;
    wgpu_adapter_device_id_ = renderer_info.device_id;
    wgpu_supports_16bit_norm_ =
        renderer_info.supports_texture_format_16bit_norm != 0;
  } else {
    spdlog::warn("[WgpuMetal] failed to query renderer adapter info");
  }
  return true;
}

void WgpuMetalPresentationBackend::shutdown() {
  if (async_state_) {
    std::unique_lock<std::mutex> lock(async_state_->mutex);
    async_state_->shutdown = true;
    async_state_->backend = nullptr;
    if (!g_in_wgpu_async_completion_callback) {
      async_state_->cv.wait(lock, [&] {
        return async_state_->active_callbacks == 0;
      });
    }
  }
  if (wgpu_renderer_) {
    VPWgpuMetalRendererDestroy(wgpu_renderer_);
    wgpu_renderer_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    release_target_texture_cache_locked();
    release_source_texture_cache_locked();
    if (external_flutter_texture_) {
      CFRelease(external_flutter_texture_);
      external_flutter_texture_ = nullptr;
    }
    external_flutter_width_ = 0;
    external_flutter_height_ = 0;
    external_flutter_pixel_format_ = 0;
    external_flutter_surface_last_error_ = "cleared";
    draw_target_pixel_buffer_ = nullptr;
    draw_target_is_metal_texture_ = false;
    target_ring_.clear();
    target_ring_enabled_ = false;
    in_flight_draws_ = 0;
    ++target_ring_generation_;
    displayed_target_address_ = 0;
    protected_target_address_ = 0;
  }
  if (texture_cache_) {
    CFRelease(texture_cache_);
    texture_cache_ = nullptr;
  }
  if (metal_device_) {
    CFRelease(metal_device_);
    metal_device_ = nullptr;
  }
  width_ = 0;
  height_ = 0;
  draw_target_width_ = 0;
  draw_target_height_ = 0;
  draw_target_output_format_ = 0;
  draw_target_output_color_mode_ = 0;
  draw_target_is_metal_texture_ = false;
  draw_target_render_format_ = "unknown";
  draw_target_color_space_ = "unknown";
  headless_ = true;
  wgpu_adapter_description_ = "unknown";
  wgpu_driver_type_ = "unknown";
  wgpu_backend_name_ = "unknown";
  wgpu_device_type_ = "unknown";
  wgpu_adapter_vendor_id_ = 0;
  wgpu_adapter_device_id_ = 0;
  wgpu_supports_16bit_norm_ = false;
  retained_source_available_ = false;
  retained_source_submitted_generation_ = 0;
  retained_source_committed_generation_ = 0;
  last_source_signature_ = 0;
  retained_source_frame_info_available_ = false;
  retained_source_frame_info_ = {};
}

bool WgpuMetalPresentationBackend::available() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return available_locked();
}

bool WgpuMetalPresentationBackend::available_locked() const {
  return metal_device_ && texture_cache_ && wgpu_renderer_ && wgpu_ffi_available();
}

void WgpuMetalPresentationBackend::release_target_texture_cache_for_slot(
    TargetSlot& slot) {
  if (slot.cached_texture_ref) {
    CFRelease(slot.cached_texture_ref);
    slot.cached_texture_ref = nullptr;
  }
  slot.cached_width = 0;
  slot.cached_height = 0;
  slot.cached_pixel_format = 0;
}

void WgpuMetalPresentationBackend::release_target_texture_cache_locked() {
  if (single_target_texture_ref_) {
    CFRelease(single_target_texture_ref_);
    single_target_texture_ref_ = nullptr;
  }
  single_target_texture_width_ = 0;
  single_target_texture_height_ = 0;
  single_target_texture_pixel_format_ = 0;
  for (auto& slot : target_ring_) {
    release_target_texture_cache_for_slot(slot);
  }
}

void WgpuMetalPresentationBackend::release_source_texture_cache_locked() {
  for (auto& entry : source_texture_cache_) {
    if (entry.texture_ref) {
      CFRelease(entry.texture_ref);
      entry.texture_ref = nullptr;
    }
  }
  source_texture_cache_.clear();
  source_texture_cache_clock_ = 0;
}

void* WgpuMetalPresentationBackend::cached_target_texture_ref(
    void* pixel_buffer,
    uint64_t metal_pixel_format,
    int32_t width,
    int32_t height,
    std::string& error) {
  if (!pixel_buffer || !texture_cache_ || width <= 0 || height <= 0) {
    error = "wgpu-metal target texture cache arguments are invalid";
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  void** texture_ref_slot = &single_target_texture_ref_;
  int32_t* cached_width = &single_target_texture_width_;
  int32_t* cached_height = &single_target_texture_height_;
  uint64_t* cached_pixel_format = &single_target_texture_pixel_format_;
  if (target_ring_enabled_) {
    texture_ref_slot = nullptr;
    for (auto& slot : target_ring_) {
      if (slot.pixel_buffer != pixel_buffer) {
        continue;
      }
      texture_ref_slot = &slot.cached_texture_ref;
      cached_width = &slot.cached_width;
      cached_height = &slot.cached_height;
      cached_pixel_format = &slot.cached_pixel_format;
      break;
    }
    if (!texture_ref_slot) {
      error = "wgpu-metal target texture cache slot is unavailable";
      return nullptr;
    }
  }
  if (*texture_ref_slot &&
      (*cached_width != width || *cached_height != height ||
       *cached_pixel_format != metal_pixel_format)) {
    CFRelease(*texture_ref_slot);
    *texture_ref_slot = nullptr;
  }
  if (!*texture_ref_slot) {
    CVMetalTextureRef texture_ref = nullptr;
    const CVReturn status = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault,
        static_cast<CVMetalTextureCacheRef>(texture_cache_),
        as_pixel_buffer(pixel_buffer),
        nullptr,
        static_cast<MTLPixelFormat>(metal_pixel_format),
        width,
        height,
        0,
        &texture_ref);
    if (status != kCVReturnSuccess || !texture_ref) {
      if (texture_ref) {
        CFRelease(texture_ref);
      }
      error = "wgpu-metal failed to wrap target CVPixelBuffer";
      return nullptr;
    }
    *texture_ref_slot = texture_ref;
    *cached_width = width;
    *cached_height = height;
    *cached_pixel_format = metal_pixel_format;
  }
  return const_cast<void*>(CFRetain(*texture_ref_slot));
}

void* WgpuMetalPresentationBackend::cached_source_texture_ref(
    void* pixel_buffer,
    uint64_t metal_pixel_format,
    int32_t width,
    int32_t height,
    size_t plane,
    std::string& error) {
  if (!pixel_buffer || !texture_cache_ || width <= 0 || height <= 0) {
    error = "wgpu-metal source texture cache arguments are invalid";
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  source_texture_cache_clock_ = source_texture_cache_clock_ + 1;
  for (auto& entry : source_texture_cache_) {
    if (entry.pixel_buffer == pixel_buffer &&
        entry.pixel_format == metal_pixel_format &&
        entry.width == width &&
        entry.height == height &&
        entry.plane == plane &&
        entry.texture_ref) {
      entry.last_used = source_texture_cache_clock_;
      return const_cast<void*>(CFRetain(entry.texture_ref));
    }
  }

  CVMetalTextureRef texture_ref = nullptr;
  const CVReturn status = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      static_cast<CVMetalTextureCacheRef>(texture_cache_),
      as_pixel_buffer(pixel_buffer),
      nullptr,
      static_cast<MTLPixelFormat>(metal_pixel_format),
      width,
      height,
      plane,
      &texture_ref);
  if (status != kCVReturnSuccess || !texture_ref) {
    if (texture_ref) {
      CFRelease(texture_ref);
    }
    error = "wgpu-metal failed to wrap source CVPixelBuffer plane";
    return nullptr;
  }
  source_texture_cache_.push_back(SourceTextureCacheEntry{
      pixel_buffer,
      texture_ref,
      metal_pixel_format,
      width,
      height,
      plane,
      source_texture_cache_clock_,
  });
  constexpr size_t kMaxSourceTextureCacheEntries = 12;
  if (source_texture_cache_.size() > kMaxSourceTextureCacheEntries) {
    const auto evict = std::min_element(
        source_texture_cache_.begin(),
        source_texture_cache_.end(),
        [](const SourceTextureCacheEntry& lhs,
           const SourceTextureCacheEntry& rhs) {
          return lhs.last_used < rhs.last_used;
        });
    if (evict != source_texture_cache_.end()) {
      if (evict->texture_ref) {
        CFRelease(evict->texture_ref);
      }
      source_texture_cache_.erase(evict);
    }
  }
  return const_cast<void*>(CFRetain(texture_ref));
}

bool WgpuMetalPresentationBackend::update_headless_output(void* output,
                                                          int width,
                                                          int height,
                                                          int max_track_slots) {
  if (!output || width <= 0 || height <= 0) {
    clear_headless_output();
    return false;
  }
  WgpuOutputTargetDescriptor descriptor;
  std::string target_error;
  if (!resolve_output_target_descriptor(output,
                                        width,
                                        height,
                                        descriptor,
                                        target_error)) {
    set_last_error(target_error.empty()
                       ? "wgpu-metal presentation target is invalid"
                       : target_error);
    return false;
  }
  if (!validate_target_texture_device(texture_cache_,
                                      output,
                                      descriptor.metal_pixel_format,
                                      width,
                                      height,
                                      metal_device_,
                                      target_error)) {
    set_last_error(target_error.empty()
                       ? "wgpu-metal presentation target is invalid"
                       : target_error);
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  release_target_texture_cache_locked();
  target_ring_.clear();
  target_ring_enabled_ = false;
  displayed_target_address_ = 0;
  protected_target_address_ = 0;
  ++target_ring_generation_;
  draw_target_output_format_ = descriptor.ffi_output_format;
  draw_target_output_color_mode_ = descriptor.ffi_output_color_mode;
  draw_target_render_format_ = descriptor.render_target_format;
  draw_target_color_space_ = descriptor.render_color_space;
  draw_target_pixel_buffer_ = output;
  draw_target_is_metal_texture_ = false;
  width_ = width;
  height_ = height;
  draw_target_width_ = width;
  draw_target_height_ = height;
  draw_target_max_track_slots_ = std::max(1, max_track_slots);
  last_error_.clear();
  return metal_device_ && texture_cache_;
}

bool WgpuMetalPresentationBackend::update_headless_metal_texture_output(
    const vr::PresentationExternalMetalRenderTarget& target) {
  if (!target.texture || target.width <= 0 || target.height <= 0) {
    clear_headless_output();
    return false;
  }
  id<MTLTexture> texture = (__bridge id<MTLTexture>)target.texture;
  WgpuOutputTargetDescriptor descriptor;
  std::string target_error;
  if (!resolve_metal_texture_target_descriptor(texture,
                                               target.width,
                                               target.height,
                                               descriptor,
                                               target_error)) {
    set_last_error(target_error.empty()
                       ? "wgpu-metal Metal texture presentation target is invalid"
                       : target_error);
    return false;
  }
  if (!metal_texture_matches_device(texture, metal_device_)) {
    set_last_error(
        "wgpu-metal Metal texture target device does not match wgpu Metal device");
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  release_target_texture_cache_locked();
  target_ring_.clear();
  target_ring_enabled_ = false;
  displayed_target_address_ = 0;
  protected_target_address_ = 0;
  ++target_ring_generation_;
  draw_target_output_format_ = descriptor.ffi_output_format;
  draw_target_output_color_mode_ = descriptor.ffi_output_color_mode;
  draw_target_render_format_ = descriptor.render_target_format;
  draw_target_color_space_ = descriptor.render_color_space;
  draw_target_pixel_buffer_ = target.texture;
  draw_target_is_metal_texture_ = true;
  width_ = target.width;
  height_ = target.height;
  draw_target_width_ = target.width;
  draw_target_height_ = target.height;
  draw_target_viewport_left_ = std::clamp(target.viewport_left, 0.0f, 1.0f);
  draw_target_viewport_top_ = std::clamp(target.viewport_top, 0.0f, 1.0f);
  draw_target_viewport_right_ =
      std::clamp(target.viewport_right, draw_target_viewport_left_, 1.0f);
  draw_target_viewport_bottom_ =
      std::clamp(target.viewport_bottom, draw_target_viewport_top_, 1.0f);
  if (draw_target_viewport_right_ <= draw_target_viewport_left_ ||
      draw_target_viewport_bottom_ <= draw_target_viewport_top_) {
    draw_target_viewport_left_ = 0.0f;
    draw_target_viewport_top_ = 0.0f;
    draw_target_viewport_right_ = 1.0f;
    draw_target_viewport_bottom_ = 1.0f;
  }
  draw_target_max_track_slots_ = std::max(1, target.max_track_slots);
  last_error_.clear();
  return metal_device_ && texture_cache_;
}

bool WgpuMetalPresentationBackend::update_headless_output_ring(
    const void* const* pixel_buffers,
    size_t pixel_buffer_count,
    void* displayed_pixel_buffer,
    void* protected_pixel_buffer,
    int width,
    int height,
    int max_track_slots) {
  if (!pixel_buffers || pixel_buffer_count == 0 || width <= 0 || height <= 0) {
    clear_headless_output();
    return false;
  }
  const uint64_t displayed_address = pointer_bits(displayed_pixel_buffer);
  const uint64_t protected_address = pointer_bits(protected_pixel_buffer);
  WgpuOutputTargetDescriptor ring_descriptor;
  std::string ring_target_error;
  bool ring_descriptor_available = false;
  std::vector<TargetSlot> next_ring;
  next_ring.reserve(pixel_buffer_count);
  std::vector<uint64_t> seen_addresses;
  seen_addresses.reserve(pixel_buffer_count);
  for (size_t i = 0; i < pixel_buffer_count; ++i) {
    if (!pixel_buffers[i]) {
      set_last_error("wgpu-metal target ring contains null pixel buffers");
      return false;
    }
    void* pixel_buffer = const_cast<void*>(pixel_buffers[i]);
    const uint64_t address = pointer_bits(pixel_buffer);
    if (std::find(seen_addresses.begin(), seen_addresses.end(), address) !=
        seen_addresses.end()) {
      set_last_error("wgpu-metal target ring contains duplicate pixel buffers");
      return false;
    }
    seen_addresses.push_back(address);
    WgpuOutputTargetDescriptor descriptor;
    if (!resolve_output_target_descriptor(pixel_buffer,
                                          width,
                                          height,
                                          descriptor,
                                          ring_target_error) ||
        !validate_target_texture_device(texture_cache_,
                                        pixel_buffer,
                                        descriptor.metal_pixel_format,
                                        width,
                                        height,
                                        metal_device_,
                                        ring_target_error)) {
      set_last_error(ring_target_error.empty()
                         ? "wgpu-metal target ring descriptor is invalid"
                         : ring_target_error);
      return false;
    }
    if (!ring_descriptor_available) {
      ring_descriptor = descriptor;
      ring_descriptor_available = true;
    } else if (descriptor.metal_pixel_format != ring_descriptor.metal_pixel_format ||
               descriptor.ffi_output_format != ring_descriptor.ffi_output_format ||
               descriptor.ffi_output_color_mode !=
                   ring_descriptor.ffi_output_color_mode) {
      set_last_error("wgpu-metal target ring contains mixed target formats");
      return false;
    }
    TargetSlot slot;
    slot.pixel_buffer = pixel_buffer;
    if (address == displayed_address) {
      slot.state = TargetState::Displayed;
    } else if (address == protected_address) {
      slot.state = TargetState::Protected;
    } else {
      slot.state = TargetState::Available;
    }
    next_ring.push_back(slot);
  }
  if (next_ring.empty() || !ring_descriptor_available) {
    set_last_error("wgpu-metal target ring is empty");
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const int clamped_track_slots = std::max(1, max_track_slots);
  const bool same_ring =
      target_ring_.size() == next_ring.size() &&
      draw_target_width_ == width &&
      draw_target_height_ == height &&
      draw_target_max_track_slots_ == clamped_track_slots &&
      draw_target_output_format_ == ring_descriptor.ffi_output_format &&
      draw_target_output_color_mode_ == ring_descriptor.ffi_output_color_mode &&
      draw_target_render_format_ == ring_descriptor.render_target_format &&
      draw_target_color_space_ == ring_descriptor.render_color_space &&
      std::equal(
          target_ring_.begin(),
          target_ring_.end(),
          next_ring.begin(),
          [](const TargetSlot& current, const TargetSlot& next) {
            return current.pixel_buffer == next.pixel_buffer;
          });
  if (same_ring) {
    displayed_target_address_ = displayed_address;
    protected_target_address_ = protected_address;
    target_ring_enabled_ = !target_ring_.empty();
    draw_target_pixel_buffer_ = nullptr;
    draw_target_is_metal_texture_ = false;
    for (auto& slot : target_ring_) {
      if (slot.state == TargetState::InFlight) {
        continue;
      }
      const uint64_t address = pointer_bits(slot.pixel_buffer);
      if (address == displayed_target_address_) {
        slot.state = TargetState::Displayed;
      } else if (address == protected_target_address_) {
        slot.state = TargetState::Protected;
      } else if (slot.state == TargetState::Displayed ||
                 slot.state == TargetState::Protected) {
        slot.state = TargetState::Available;
      }
    }
    last_error_.clear();
    return metal_device_ && texture_cache_;
  }
  release_target_texture_cache_locked();
  ++target_ring_generation_;
  for (auto& slot : next_ring) {
    slot.slot_id = ++next_target_slot_id_;
  }
  target_ring_ = std::move(next_ring);
  displayed_target_address_ = displayed_address;
  protected_target_address_ = protected_address;
  target_ring_enabled_ = !target_ring_.empty();
  draw_target_pixel_buffer_ = nullptr;
  draw_target_is_metal_texture_ = false;
  draw_target_output_format_ = ring_descriptor.ffi_output_format;
  draw_target_output_color_mode_ = ring_descriptor.ffi_output_color_mode;
  draw_target_render_format_ = ring_descriptor.render_target_format;
  draw_target_color_space_ = ring_descriptor.render_color_space;
  width_ = width;
  height_ = height;
  draw_target_width_ = width;
  draw_target_height_ = height;
  draw_target_viewport_left_ = 0.0f;
  draw_target_viewport_top_ = 0.0f;
  draw_target_viewport_right_ = 1.0f;
  draw_target_viewport_bottom_ = 1.0f;
  draw_target_max_track_slots_ = clamped_track_slots;
  last_error_.clear();
  return metal_device_ && texture_cache_;
}

void WgpuMetalPresentationBackend::mark_headless_output_displayed(void* pixel_buffer) {
  if (!pixel_buffer) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!target_ring_enabled_) {
    return;
  }
  displayed_target_address_ = pointer_bits(pixel_buffer);
  for (auto& slot : target_ring_) {
    const uint64_t address = pointer_bits(slot.pixel_buffer);
    if (slot.state == TargetState::InFlight) {
      continue;
    }
    if (address == displayed_target_address_) {
      slot.state = TargetState::Displayed;
    } else if (address == protected_target_address_) {
      slot.state = TargetState::Protected;
    } else if (slot.state == TargetState::Displayed ||
               slot.state == TargetState::Protected) {
      slot.state = TargetState::Available;
    }
  }
}

void WgpuMetalPresentationBackend::protect_headless_output(void* pixel_buffer) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!target_ring_enabled_) {
    return;
  }
  protected_target_address_ = pointer_bits(pixel_buffer);
  for (auto& slot : target_ring_) {
    const uint64_t address = pointer_bits(slot.pixel_buffer);
    if (slot.state == TargetState::InFlight) {
      continue;
    }
    if (address == displayed_target_address_) {
      slot.state = TargetState::Displayed;
    } else if (pixel_buffer && address == protected_target_address_) {
      slot.state = TargetState::Protected;
    } else if (slot.state == TargetState::Protected) {
      slot.state = TargetState::Available;
    }
  }
}

void WgpuMetalPresentationBackend::release_headless_output(void* pixel_buffer) {
  if (!pixel_buffer) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!target_ring_enabled_) {
    return;
  }
  const uint64_t released = pointer_bits(pixel_buffer);
  for (auto& slot : target_ring_) {
    if (pointer_bits(slot.pixel_buffer) != released) {
      continue;
    }
    if (slot.state == TargetState::InFlight) {
      return;
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

void WgpuMetalPresentationBackend::clear_headless_output() {
  std::lock_guard<std::mutex> lock(mutex_);
  release_target_texture_cache_locked();
  draw_target_pixel_buffer_ = nullptr;
  draw_target_is_metal_texture_ = false;
  target_ring_.clear();
  target_ring_enabled_ = false;
  in_flight_draws_ = 0;
  ++target_ring_generation_;
  displayed_target_address_ = 0;
  protected_target_address_ = 0;
  draw_target_width_ = 0;
  draw_target_height_ = 0;
  draw_target_viewport_left_ = 0.0f;
  draw_target_viewport_top_ = 0.0f;
  draw_target_viewport_right_ = 1.0f;
  draw_target_viewport_bottom_ = 1.0f;
  draw_target_output_format_ = 0;
  draw_target_output_color_mode_ = 0;
  draw_target_render_format_ = "unknown";
  draw_target_color_space_ = "unknown";
}

void* WgpuMetalPresentationBackend::native_render_device() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metal_device_;
}

bool WgpuMetalPresentationBackend::update_external_flutter_metal_surface(
    const vr::PresentationExternalMetalSurface& surface) {
  if (!surface.texture || surface.width <= 0 || surface.height <= 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    external_flutter_surface_last_error_ = "invalid-arguments";
    return false;
  }
  id<MTLTexture> texture = (__bridge id<MTLTexture>)surface.texture;
  if (!texture) {
    std::lock_guard<std::mutex> lock(mutex_);
    external_flutter_surface_last_error_ = "invalid-texture";
    return false;
  }
  if (texture.pixelFormat != MTLPixelFormatBGRA8Unorm ||
      surface.pixel_format != static_cast<uint64_t>(MTLPixelFormatBGRA8Unorm)) {
    std::lock_guard<std::mutex> lock(mutex_);
    external_flutter_surface_last_error_ = "unsupported-format";
    return false;
  }
  if (texture.width != static_cast<NSUInteger>(surface.width) ||
      texture.height != static_cast<NSUInteger>(surface.height)) {
    std::lock_guard<std::mutex> lock(mutex_);
    external_flutter_surface_last_error_ = "dimension-mismatch";
    return false;
  }
  if (!metal_texture_matches_device(texture, metal_device_)) {
    std::lock_guard<std::mutex> lock(mutex_);
    external_flutter_surface_last_error_ = "device-mismatch";
    return false;
  }

  void* retained_texture = (__bridge_retained void*)texture;
  std::lock_guard<std::mutex> lock(mutex_);
  if (external_flutter_texture_) {
    CFRelease(external_flutter_texture_);
  }
  external_flutter_texture_ = retained_texture;
  external_flutter_width_ = surface.width;
  external_flutter_height_ = surface.height;
  external_flutter_pixel_format_ = surface.pixel_format;
  external_flutter_surface_generation_ = surface.frame_generation;
  ++external_flutter_surface_update_count_;
  external_flutter_surface_last_error_ = "none";
  return true;
}

void WgpuMetalPresentationBackend::clear_external_flutter_metal_surface() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (external_flutter_texture_) {
    CFRelease(external_flutter_texture_);
    external_flutter_texture_ = nullptr;
  }
  external_flutter_width_ = 0;
  external_flutter_height_ = 0;
  external_flutter_pixel_format_ = 0;
  external_flutter_surface_last_error_ = "cleared";
}

bool WgpuMetalPresentationBackend::update_source_projection(
    const vr::PresentationSourceProjection& projection) {
  std::lock_guard<std::mutex> lock(mutex_);
  source_projection_ = projection;
  source_projection_.enabled = projection.enabled;
  ++source_projection_update_count_;
  return source_projection_.enabled;
}

void WgpuMetalPresentationBackend::clear_source_projection() {
  std::lock_guard<std::mutex> lock(mutex_);
  source_projection_ = {};
}

vr::PresentationBackendStats WgpuMetalPresentationBackend::presentation_stats() const {
  vr::PresentationBackendStats stats;
  std::lock_guard<std::mutex> lock(mutex_);
  stats.cvpixelbuffer_upload_count =
      static_cast<int64_t>(cvpixelbuffer_upload_count_);
  stats.present_package_upload_count =
      static_cast<int64_t>(present_package_upload_count_);
  stats.last_present_package_copy_us =
      static_cast<int64_t>(last_present_package_copy_us_);
  stats.last_present_package_gpu_wait_us =
      static_cast<int64_t>(last_present_package_gpu_wait_us_);
  stats.last_present_package_total_us =
      static_cast<int64_t>(last_present_package_total_us_);
  stats.last_present_package_storage = last_present_package_storage_;
  stats.backend_available = available_locked() ? 1 : 0;
  stats.target_installed = target_installed_locked() ? 1 : 0;
  stats.last_draw_succeeded = last_draw_succeeded_ ? 1 : 0;
  stats.draw_failure_count = draw_failure_count_;
  stats.consecutive_draw_failures = consecutive_draw_failures_;
  stats.last_successful_frame_pts_us = last_frame_info_.pts_us;
  stats.staging_allocation_count = staging_allocation_count_;
  stats.staging_reuse_count = staging_reuse_count_;
  stats.staging_max_bytes = staging_max_bytes_;
  stats.in_flight_metal_buffer_count = in_flight_draws_;
  stats.metal_buffer_exhaustion_count = target_ring_backpressure_count_;
  stats.metal_command_completion_p95_us = metal_command_completion_p95_us_;
  stats.metal_command_failure_count = metal_command_failure_count_;
  stats.wgpu_compose_total_p95_us = wgpu_compose_total_p95_us_;
  stats.wgpu_compose_pre_render_p95_us = wgpu_compose_pre_render_p95_us_;
  stats.wgpu_compose_import_p95_us = wgpu_compose_import_p95_us_;
  stats.wgpu_compose_prepare_p95_us = wgpu_compose_prepare_p95_us_;
  stats.wgpu_compose_overlay_encode_p95_us =
      wgpu_compose_overlay_encode_p95_us_;
  stats.wgpu_compose_bind_group_p95_us = wgpu_compose_bind_group_p95_us_;
  stats.wgpu_compose_pass_encode_p95_us = wgpu_compose_pass_encode_p95_us_;
  stats.wgpu_compose_submit_p95_us = wgpu_compose_submit_p95_us_;
  stats.wgpu_compose_cpu_render_p95_us = wgpu_compose_cpu_render_p95_us_;
  stats.async_metal_publish_active = 1;
  stats.overlay_last_expected = overlay_last_expected_ ? 1 : 0;
  stats.overlay_last_applied = overlay_last_applied_ ? 1 : 0;
  stats.overlay_last_fill_rect_count = overlay_last_fill_rect_count_;
  stats.overlay_last_line_rect_count = overlay_last_line_rect_count_;
  stats.overlay_expected_count = overlay_expected_count_;
  stats.overlay_applied_count = overlay_applied_count_;
  stats.overlay_missed_count = overlay_missed_count_;
  stats.overlay_gpu_success_count = overlay_gpu_success_count_;
  stats.overlay_gpu_failure_count = overlay_gpu_failure_count_;
  stats.video_source_update_count = video_source_update_count_;
  stats.viewport_composite_count = viewport_composite_count_;
  stats.source_frame_cache_hit_count = source_frame_cache_hit_count_;
  stats.source_frame_cache_miss_count = source_frame_cache_miss_count_;
  return stats;
}

vr::PresentationBackendDiagnostics WgpuMetalPresentationBackend::diagnostics() const {
  vr::PresentationBackendDiagnostics diagnostics;
  std::lock_guard<std::mutex> lock(mutex_);
  VPWgpuMetalProfilerSnapshot profiler = {};
  if (wgpu_renderer_) {
    (void)VPWgpuMetalRendererGetProfilerSnapshot(wgpu_renderer_, &profiler);
  }
  diagnostics.backend = "wgpu-metal";
  diagnostics.target_format = "CVPixelBuffer/MTLTexture";
  diagnostics.render_target_format = draw_target_render_format_;
  diagnostics.render_color_space = draw_target_color_space_;
  diagnostics.fallback_reason = last_error_.empty() ? "none" : last_error_;
  diagnostics.adapter_description = wgpu_adapter_description_;
  diagnostics.driver_type = wgpu_backend_name_ + "/" + wgpu_device_type_ +
      "/" + wgpu_driver_type_ +
      (wgpu_supports_16bit_norm_ ? "/texture-format-16bit-norm" : "");
  diagnostics.adapter_vendor_id = static_cast<int32_t>(wgpu_adapter_vendor_id_);
  diagnostics.adapter_device_id = static_cast<int32_t>(wgpu_adapter_device_id_);
  diagnostics.feature_level = VPWgpuFfiVersion();
  diagnostics.width = draw_target_width_;
  diagnostics.height = draw_target_height_;
  diagnostics.buffer_count = target_ring_enabled_
      ? static_cast<int32_t>(target_ring_.size())
      : (draw_target_pixel_buffer_ ? 1 : 0);
  diagnostics.source_cache_active = retained_source_available_;
  diagnostics.source_cache_format = "wgpu-owned-ring";
  diagnostics.source_cache_texture_count =
      static_cast<int32_t>(profiler.imported_texture_cache_size);
  diagnostics.source_cache_generation =
      profiler.destination_import_reuse_count + profiler.source_import_reuse_count;
  diagnostics.source_cache_publish_count =
      profiler.destination_import_count + profiler.source_import_count;
  diagnostics.source_cache_backpressure_count =
      profiler.imported_texture_cache_eviction_count;
  diagnostics.source_projection_active = source_projection_.enabled;
  diagnostics.source_projection_update_count =
      source_projection_update_count_;
  diagnostics.source_projection_consume_count =
      source_projection_consume_count_;
  diagnostics.external_flutter_surface_generation =
      external_flutter_surface_generation_;
  diagnostics.external_flutter_surface_consumed_generation =
      external_flutter_surface_consumed_generation_;
  diagnostics.external_flutter_surface_update_count =
      external_flutter_surface_update_count_;
  diagnostics.external_flutter_surface_consume_count =
      external_flutter_surface_consume_count_;
  diagnostics.external_flutter_surface_last_error =
      external_flutter_surface_last_error_;
  diagnostics.headless = headless_;
  return diagnostics;
}

bool WgpuMetalPresentationBackend::copy_last_frame_info(
    vr::PresentationBackendFrameInfo* out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!out || !last_frame_info_available_) {
    return false;
  }
  *out = last_frame_info_;
  return true;
}

bool WgpuMetalPresentationBackend::capture_front_buffer(std::vector<uint8_t>& bgra,
                                                        int& width,
                                                        int& height) {
  void* target = nullptr;
  bool target_is_metal_texture = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    target = capture_target_locked();
    target_is_metal_texture = draw_target_is_metal_texture_;
  }
  if (target_is_metal_texture) {
    set_last_error("wgpu-metal capture is unavailable for direct Metal texture targets");
    bgra.clear();
    width = 0;
    height = 0;
    return false;
  }
  auto* pixel_buffer = as_pixel_buffer(target);
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
  if (CVPixelBufferGetPixelFormatType(pixel_buffer) != kCVPixelFormatType_32BGRA) {
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    set_last_error("wgpu-metal capture only supports BGRA8 SDR targets");
    bgra.clear();
    width = 0;
    height = 0;
    return false;
  }
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

bool WgpuMetalPresentationBackend::capture_front_buffer_region(
    int x,
    int y,
    int width,
    int height,
    std::vector<uint8_t>& bgra,
    int& region_width,
    int& region_height) {
  bgra.clear();
  region_width = 0;
  region_height = 0;
  if (width <= 0 || height <= 0) {
    return false;
  }
  void* target = nullptr;
  bool target_is_metal_texture = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    target = capture_target_locked();
    target_is_metal_texture = draw_target_is_metal_texture_;
  }
  if (target_is_metal_texture) {
    set_last_error("wgpu-metal capture is unavailable for direct Metal texture targets");
    return false;
  }
  auto* pixel_buffer = as_pixel_buffer(target);
  if (!pixel_buffer ||
      CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly) !=
          kCVReturnSuccess) {
    return false;
  }
  const int pixel_width = static_cast<int>(CVPixelBufferGetWidth(pixel_buffer));
  const int pixel_height = static_cast<int>(CVPixelBufferGetHeight(pixel_buffer));
  if (CVPixelBufferGetPixelFormatType(pixel_buffer) != kCVPixelFormatType_32BGRA) {
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    set_last_error("wgpu-metal capture only supports BGRA8 SDR targets");
    return false;
  }
  const int stride = static_cast<int>(CVPixelBufferGetBytesPerRow(pixel_buffer));
  const auto* source =
      static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixel_buffer));
  if (!source || pixel_width <= 0 || pixel_height <= 0 ||
      stride < pixel_width * 4) {
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    return false;
  }
  const int left = std::clamp(x, 0, pixel_width);
  const int top = std::clamp(y, 0, pixel_height);
  const int right = static_cast<int>(
      std::clamp(static_cast<int64_t>(x) + width,
                 int64_t{0},
                 static_cast<int64_t>(pixel_width)));
  const int bottom = static_cast<int>(
      std::clamp(static_cast<int64_t>(y) + height,
                 int64_t{0},
                 static_cast<int64_t>(pixel_height)));
  if (right <= left || bottom <= top) {
    CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
    return false;
  }
  region_width = right - left;
  region_height = bottom - top;
  const size_t row_bytes = static_cast<size_t>(region_width) * 4u;
  bgra.resize(row_bytes * static_cast<size_t>(region_height));
  for (int row = 0; row < region_height; ++row) {
    const auto* src_row =
        source + static_cast<size_t>(top + row) * static_cast<size_t>(stride) +
        static_cast<size_t>(left) * 4u;
    std::memcpy(bgra.data() + static_cast<size_t>(row) * row_bytes,
                src_row,
                row_bytes);
  }
  CVPixelBufferUnlockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly);
  return true;
}

bool WgpuMetalPresentationBackend::draw_frame(
    const vr::RendererDrawSnapshot& snapshot,
    const vr::PresentationBackendDrawHooks& hooks) {
  const auto draw_start = std::chrono::steady_clock::now();
  set_last_error("");
  if (!wgpu_ffi_available() || !wgpu_renderer_) {
    mark_draw_failure("wgpu-metal Rust FFI is not linked");
    return false;
  }
  TargetAcquireResult target_lease;
  void* target = nullptr;
  bool target_is_metal_texture = false;
  bool target_acquired = false;
  std::string acquire_error;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    target_lease = acquire_draw_target_locked(hooks.draw_source);
    target = target_lease.pixel_buffer;
    target_is_metal_texture = draw_target_is_metal_texture_;
    target_acquired = target != nullptr;
    acquire_error = last_error_;
  }
  const uint64_t acquired_target_address = pointer_bits(target);
  auto complete_acquired_target = [&](bool success) {
    if (!target_acquired || acquired_target_address == 0) {
      return;
    }
    complete_draw_target(acquired_target_address,
                         target_lease.ring_generation,
                         target_lease.slot_id,
                         success);
    target_acquired = false;
  };
  auto fail_after_target_acquire = [&](std::string error) {
    complete_acquired_target(false);
    mark_draw_failure(std::move(error));
    return false;
  };
  if (!target && !acquire_error.empty()) {
    mark_draw_failure(acquire_error);
    return false;
  }
  if (!metal_device_ || !texture_cache_ || !target ||
      draw_target_width_ <= 0 || draw_target_height_ <= 0) {
    return fail_after_target_acquire("wgpu-metal presentation target is unavailable");
  }
  WgpuOutputTargetDescriptor output_target;
  std::string target_error;
  if (target_is_metal_texture) {
    id<MTLTexture> texture = (__bridge id<MTLTexture>)target;
    if (!resolve_metal_texture_target_descriptor(texture,
                                                 draw_target_width_,
                                                 draw_target_height_,
                                                 output_target,
                                                 target_error) ||
        !metal_texture_matches_device(texture, metal_device_)) {
      return fail_after_target_acquire(
          target_error.empty()
              ? "wgpu-metal Metal texture presentation target is invalid"
              : target_error);
    }
  } else {
    if (!resolve_output_target_descriptor(target,
                                          draw_target_width_,
                                          draw_target_height_,
                                          output_target,
                                          target_error)) {
      return fail_after_target_acquire(target_error);
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    draw_target_output_format_ = output_target.ffi_output_format;
    draw_target_output_color_mode_ = output_target.ffi_output_color_mode;
    draw_target_render_format_ = output_target.render_target_format;
    draw_target_color_space_ = output_target.render_color_space;
  }
  const float viewport_left =
      std::clamp(draw_target_viewport_left_, 0.0f, 1.0f);
  const float viewport_top =
      std::clamp(draw_target_viewport_top_, 0.0f, 1.0f);
  const float viewport_right =
      std::clamp(draw_target_viewport_right_, viewport_left, 1.0f);
  const float viewport_bottom =
      std::clamp(draw_target_viewport_bottom_, viewport_top, 1.0f);
  const int32_t viewport_width = std::max(
      1,
      static_cast<int32_t>(std::lround(
          (viewport_right - viewport_left) *
          static_cast<float>(draw_target_width_))));
  const int32_t viewport_height = std::max(
      1,
      static_cast<int32_t>(std::lround(
          (viewport_bottom - viewport_top) *
          static_cast<float>(draw_target_height_))));
  const float viewport_rect_pixels[4] = {
      viewport_left * static_cast<float>(draw_target_width_),
      viewport_top * static_cast<float>(draw_target_height_),
      static_cast<float>(viewport_width),
      static_cast<float>(viewport_height),
  };
  auto apply_viewport_rect = [&](auto& request) {
    request.viewport_left = viewport_left;
    request.viewport_top = viewport_top;
    request.viewport_right = viewport_right;
    request.viewport_bottom = viewport_bottom;
  };
  auto resolve_destination_texture =
      [&](CVMetalTextureRef& destination_ref,
          void*& destination_texture,
          std::string& destination_error) -> bool {
    destination_ref = nullptr;
    destination_texture = nullptr;
    id<MTLTexture> metal_texture = nil;
    if (target_is_metal_texture) {
      metal_texture = (__bridge id<MTLTexture>)target;
    } else {
      destination_ref = static_cast<CVMetalTextureRef>(cached_target_texture_ref(
          target,
          static_cast<uint64_t>(output_target.metal_pixel_format),
          draw_target_width_,
          draw_target_height_,
          destination_error));
      if (!destination_ref) {
        if (destination_error.empty()) {
          destination_error = "wgpu-metal failed to wrap target CVPixelBuffer";
        }
        return false;
      }
      metal_texture = CVMetalTextureGetTexture(destination_ref);
    }
    if (!metal_texture_matches_device(metal_texture, metal_device_)) {
      if (destination_ref) {
        CFRelease(destination_ref);
        destination_ref = nullptr;
      }
      destination_error =
          "wgpu-metal destination texture device does not match wgpu Metal device";
      return false;
    }
    destination_texture = (__bridge void*)metal_texture;
    return true;
  };
  ExternalFlutterSurfaceSnapshot flutter_surface;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (external_flutter_texture_ &&
        external_flutter_width_ > 0 &&
        external_flutter_height_ > 0 &&
        external_flutter_pixel_format_ ==
            static_cast<uint64_t>(MTLPixelFormatBGRA8Unorm)) {
      flutter_surface.texture_ref =
          const_cast<void*>(CFRetain(external_flutter_texture_));
      flutter_surface.width = external_flutter_width_;
      flutter_surface.height = external_flutter_height_;
      flutter_surface.frame_generation = external_flutter_surface_generation_;
    }
  }
  auto apply_external_flutter_surface = [&](auto& request) {
    if (!flutter_surface.valid()) {
      return;
    }
    request.flutter_mtl_texture = flutter_surface.texture_ref;
    request.flutter_width = flutter_surface.width;
    request.flutter_height = flutter_surface.height;
  };
  auto mark_external_flutter_surface_consumed = [&]() {
    if (!flutter_surface.valid()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    external_flutter_surface_consumed_generation_ =
        flutter_surface.frame_generation;
    ++external_flutter_surface_consume_count_;
    external_flutter_surface_last_error_ = "none";
  };
  auto apply_source_projection = [&](VPMacOSNativePresentDecisionInfo& decision) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!source_projection_.enabled) {
      return;
    }
    apply_source_projection_to_macos_decision(source_projection_, decision);
    ++source_projection_consume_count_;
  };

  const int32_t track_slots = std::clamp(draw_target_max_track_slots_,
                                         1,
                                         static_cast<int>(VPMacOSNativeMaxTracks));
  const auto source_metrics = record_source_metrics(
      snapshot,
      hooks,
      viewport_width,
      viewport_height,
      track_slots,
      output_target.ffi_output_format,
      output_target.ffi_output_color_mode);
  bool retained_source_available = false;
  int32_t retained_source_storage = 0;
  bool retained_source_frame_info_available = false;
  vr::PresentationBackendFrameInfo retained_source_frame_info;
  uint64_t retained_source_generation = 0;
  uint64_t retained_source_signature = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    retained_source_available = retained_source_available_;
    retained_source_storage = last_present_package_storage_;
    retained_source_frame_info_available = retained_source_frame_info_available_;
    retained_source_frame_info = retained_source_frame_info_;
    retained_source_generation = retained_source_committed_generation_;
    retained_source_signature = last_source_signature_;
  }

  const auto overlay_primitives =
      build_overlay_primitives_for_wgpu(snapshot, hooks);
  const bool overlay_expected = overlay_primitives_expected(overlay_primitives);
  auto make_async_pending = [&](vr::PresentationBackendFrameInfo frame_info,
                                uint64_t package_copy_us,
                                int32_t package_storage,
                                uint64_t source_generation,
                                uint64_t source_signature,
                                bool source_upload) {
    auto pending = std::make_unique<AsyncDrawPending>();
    frame_info.source_generation = source_generation;
    frame_info.source_signature = source_signature;
    frame_info.source_upload = source_upload;
    pending->state = async_state_;
    pending->hooks = hooks;
    pending->frame_info = frame_info;
    pending->draw_start = draw_start;
    pending->render_call_start = draw_start;
    pending->target_pixel_buffer_address = acquired_target_address;
    pending->target_ring_generation = target_lease.ring_generation;
    pending->target_slot_id = target_lease.slot_id;
    pending->package_copy_us = package_copy_us;
    pending->package_storage = package_storage;
    pending->source_generation = source_generation;
    pending->source_signature = source_signature;
    pending->source_upload = source_upload;
    pending->target_acquired = target_acquired;
    pending->overlay_expected = overlay_expected;
    pending->overlay_fill_rect_count = overlay_primitives.fill_rects.size();
    pending->overlay_line_rect_count = overlay_primitives.line_rects.size();
    return pending;
  };

  if (source_metrics.viewport_composite && source_metrics.cache_hit &&
      retained_source_available) {
    VPMacOSNativePresentDecisionInfo retained_decision = {};
    fill_present_decision_info_from_snapshot(snapshot,
                                             viewport_width,
                                             viewport_height,
                                             &retained_decision);
    apply_source_projection(retained_decision);
    std::string destination_error;
    CVMetalTextureRef destination_ref = nullptr;
    void* destination_texture = nullptr;
    if (!resolve_destination_texture(destination_ref,
                                     destination_texture,
                                     destination_error)) {
      return fail_after_target_acquire(destination_error);
    }
    char ffi_error[256] = {};
    VPWgpuMetalRetainedCompositeRequest request = {};
    request.destination_mtl_texture = destination_texture;
    request.output_format = output_target.ffi_output_format;
    request.output_color_mode = output_target.ffi_output_color_mode;
    request.sdr_white_scale = 1.0f;
    request.decision = &retained_decision;
    request.overlay_fill_rects = overlay_primitives.fill_rects.empty()
        ? nullptr
        : overlay_primitives.fill_rects.data();
    request.overlay_fill_rect_count = overlay_primitives.fill_rects.size();
    request.overlay_line_rects = overlay_primitives.line_rects.empty()
        ? nullptr
        : overlay_primitives.line_rects.data();
    request.overlay_line_rect_count = overlay_primitives.line_rects.size();
    request.overlay_motion_lines = overlay_primitives.motion_lines.empty()
        ? nullptr
        : overlay_primitives.motion_lines.data();
    request.overlay_motion_line_count = overlay_primitives.motion_lines.size();
    request.overlay_generation = overlay_primitives.generation;
    request.width = draw_target_width_;
    request.height = draw_target_height_;
    apply_viewport_rect(request);
    request.error = ffi_error;
    request.error_size = sizeof(ffi_error);
    apply_external_flutter_surface(request);
    log_wgpu_decision("retained",
                      retained_decision,
                      viewport_rect_pixels,
                      retained_source_storage,
                      output_target.ffi_output_format,
                      output_target.ffi_output_color_mode,
                      destination_texture);
    auto frame_info = retained_source_frame_info_available
        ? retained_source_frame_info
        : frame_info_from_decision(retained_decision, target);
    frame_info.target_pixel_buffer_address = pointer_bits(target);
    auto pending =
        make_async_pending(frame_info,
                           0,
                           retained_source_storage,
                           retained_source_generation,
                           retained_source_signature,
                           false);
    pending->destination_texture_ref = destination_ref;
    destination_ref = nullptr;
    pending->render_call_start = std::chrono::steady_clock::now();
    AsyncDrawPending* pending_raw = pending.release();
    VPWgpuMetalAsyncCompletion completion = {};
    completion.callback = wgpu_async_draw_completed;
    completion.user_data = pending_raw;
    completion.profiler_snapshot = &pending_raw->profiler_snapshot;
    pending_raw->has_profiler_snapshot = true;
    const int ret = VPWgpuMetalRendererCompositeRetainedSourceAsync(
        wgpu_renderer_, &request, completion);
    if (ret != 0) {
      std::unique_ptr<AsyncDrawPending> reclaim(pending_raw);
      const std::string error =
          ffi_error[0] ? ffi_error : "wgpu-metal retained composite failed";
      return fail_after_target_acquire(error);
    }
    mark_external_flutter_surface_consumed();
    target_acquired = false;
    return true;
  }

  const auto storage_mix = snapshot_storage_mix(snapshot);
  VPMacOSNativeCVPixelBufferPresentFrameSet frame_set = {};
  std::string cv_error;
  if (snapshot_cv_pixel_buffer_frame_set(snapshot,
                                         viewport_width,
                                         viewport_height,
                                         &frame_set,
                                         cv_error)) {
    apply_source_projection(frame_set.decision);
    std::array<ScopedCVMetalTexture, VPMacOSNativeMaxTracks> source_y_refs;
    std::array<ScopedCVMetalTexture, VPMacOSNativeMaxTracks> source_uv_refs;
    VPWgpuMetalCVPixelBufferRenderRequest request = {};
    request.frame_set = &frame_set;
    for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
      if (!frame_set.decision.frames[slot].present) {
        continue;
      }
      auto* source_pixel_buffer =
          static_cast<CVPixelBufferRef>(frame_set.pixel_buffers[slot]);
      const bool is_p010 = frame_set.is_p010[slot] != 0;
      log_cv_pixel_buffer_sample(source_pixel_buffer,
                                 is_p010,
                                 slot,
                                 frame_set.decision);
      const MTLPixelFormat y_format =
          is_p010 ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
      const MTLPixelFormat uv_format =
          is_p010 ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;
      std::string source_texture_error;
      source_y_refs[slot].reset(static_cast<CVMetalTextureRef>(
          cached_source_texture_ref(
              source_pixel_buffer,
              static_cast<uint64_t>(y_format),
              static_cast<int32_t>(CVPixelBufferGetWidthOfPlane(
                  source_pixel_buffer, 0)),
              static_cast<int32_t>(CVPixelBufferGetHeightOfPlane(
                  source_pixel_buffer, 0)),
              0,
              source_texture_error)));
      source_uv_refs[slot].reset(static_cast<CVMetalTextureRef>(
          cached_source_texture_ref(
              source_pixel_buffer,
              static_cast<uint64_t>(uv_format),
              static_cast<int32_t>(CVPixelBufferGetWidthOfPlane(
                  source_pixel_buffer, 1)),
              static_cast<int32_t>(CVPixelBufferGetHeightOfPlane(
                  source_pixel_buffer, 1)),
              1,
              source_texture_error)));
      if (!source_y_refs[slot].valid() || !source_uv_refs[slot].valid()) {
        return fail_after_target_acquire(source_texture_error.empty()
                                             ? "wgpu-metal failed to wrap source CVPixelBuffer planes"
                                             : source_texture_error);
      }
      request.source_y_mtl_textures[slot] =
          (__bridge void*)source_y_refs[slot].texture();
      request.source_uv_mtl_textures[slot] =
          (__bridge void*)source_uv_refs[slot].texture();
    }

    std::string destination_error;
    CVMetalTextureRef destination_ref = nullptr;
    void* destination_texture = nullptr;
    if (!resolve_destination_texture(destination_ref,
                                     destination_texture,
                                     destination_error)) {
      return fail_after_target_acquire(destination_error);
    }
    char ffi_error[256] = {};
    request.destination_mtl_texture = destination_texture;
    request.output_format = output_target.ffi_output_format;
    request.output_color_mode = output_target.ffi_output_color_mode;
    request.sdr_white_scale = 1.0f;
    request.overlay_fill_rects = overlay_primitives.fill_rects.empty()
        ? nullptr
        : overlay_primitives.fill_rects.data();
    request.overlay_fill_rect_count = overlay_primitives.fill_rects.size();
    request.overlay_line_rects = overlay_primitives.line_rects.empty()
        ? nullptr
        : overlay_primitives.line_rects.data();
    request.overlay_line_rect_count = overlay_primitives.line_rects.size();
    request.overlay_motion_lines = overlay_primitives.motion_lines.empty()
        ? nullptr
        : overlay_primitives.motion_lines.data();
    request.overlay_motion_line_count = overlay_primitives.motion_lines.size();
    request.overlay_generation = overlay_primitives.generation;
    request.width = draw_target_width_;
    request.height = draw_target_height_;
    apply_viewport_rect(request);
    request.error = ffi_error;
    request.error_size = sizeof(ffi_error);
    apply_external_flutter_surface(request);
    log_wgpu_decision("cv-yuv",
                      frame_set.decision,
                      viewport_rect_pixels,
                      VPMacOSNativePresentPackageStorageSourceOutputAtlas,
                      output_target.ffi_output_format,
                      output_target.ffi_output_color_mode,
                      destination_texture);
    const auto frame_info = frame_info_from_decision(frame_set.decision, target);
    uint64_t source_generation = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      source_generation = ++retained_source_submitted_generation_;
    }
    auto pending = make_async_pending(
        frame_info,
        0,
        VPMacOSNativePresentPackageStorageSourceOutputAtlas,
        source_generation,
        source_metrics.signature,
        true);
    pending->destination_texture_ref = destination_ref;
    destination_ref = nullptr;
    for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
      if (frame_set.pixel_buffers[slot]) {
        pending->source_pixel_buffer_refs[slot] =
            const_cast<void*>(CFRetain(frame_set.pixel_buffers[slot]));
      }
      if (source_y_refs[slot].get()) {
        pending->source_y_texture_refs[slot] =
            const_cast<void*>(CFRetain(source_y_refs[slot].get()));
      }
      if (source_uv_refs[slot].get()) {
        pending->source_uv_texture_refs[slot] =
            const_cast<void*>(CFRetain(source_uv_refs[slot].get()));
      }
    }
    pending->render_call_start = std::chrono::steady_clock::now();
    AsyncDrawPending* pending_raw = pending.release();
    VPWgpuMetalAsyncCompletion completion = {};
    completion.callback = wgpu_async_draw_completed;
    completion.user_data = pending_raw;
    completion.profiler_snapshot = &pending_raw->profiler_snapshot;
    pending_raw->has_profiler_snapshot = true;
    const int ret = VPWgpuMetalRendererRenderCVPixelBufferFrameSetAsync(
        wgpu_renderer_, &request, completion);
    if (ret != 0) {
      std::unique_ptr<AsyncDrawPending> reclaim(pending_raw);
      const std::string error =
          ffi_error[0] ? ffi_error : "wgpu-metal render CVPixelBuffer frame set failed";
      return fail_after_target_acquire(error);
    }
    mark_external_flutter_surface_consumed();
    target_acquired = false;
    return true;
  }
  if (storage_mix.any_cv_pixel_buffer && !storage_mix.any_non_cv_pixel_buffer) {
    return fail_after_target_acquire(
        cv_error.empty() ? "wgpu-metal CVPixelBuffer frame set is invalid" : cv_error);
  }

  const auto package_layout = vr::describe_presentation_package_layout(
      viewport_width, viewport_height, track_slots);
  const size_t bgra_fallback_package_bytes =
      snapshot_bgra_fallback_package_bytes(snapshot);
  const size_t required_package_bytes =
      std::max(package_layout.max_bytes, bgra_fallback_package_bytes);
  if (required_package_bytes == 0 ||
      package_layout.bgra_row_bytes >
          static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    return fail_after_target_acquire(
        "wgpu-metal presentation package layout is invalid");
  }
  if (staging_buffer_.size() < required_package_bytes) {
    staging_buffer_.assign(required_package_bytes, 0);
    ++staging_allocation_count_;
    staging_max_bytes_ = std::max(staging_max_bytes_, staging_buffer_.size());
  } else {
    ++staging_reuse_count_;
  }
  VPMacOSNativePresentFramePackageInfo package = {};
  package.width = viewport_width;
  package.height = viewport_height;
  package.max_track_slots = track_slots;
  std::string error;
  const auto package_copy_start = std::chrono::steady_clock::now();
  // CPU/software snapshots are packed to deterministic BGRA before the wgpu
  // composite pass. Hardware VideoToolbox CVPixelBuffers use the plane-import
  // path above only to bake the current frame into the wgpu-owned source atlas;
  // retained interaction composes sample that committed atlas, not CV planes.
  fill_present_decision_info_from_snapshot(
      snapshot, viewport_width, viewport_height, &package.decision);
  apply_source_projection(package.decision);
  package.stride_bytes = static_cast<int32_t>(package_layout.bgra_row_bytes);
  package.track_stride_bytes = package_layout.bgra_track_stride_bytes;
  if (!copy_snapshot_bgra_source_package(snapshot,
                                         staging_buffer_.data(),
                                         staging_buffer_.size(),
                                         viewport_width,
                                         viewport_height,
                                         &package,
                                         error)) {
    return fail_after_target_acquire(error);
  }
  package.storage = VPMacOSNativePresentPackageStorageBGRA;
  const uint64_t package_copy_us = elapsed_us_since(package_copy_start);
  std::string destination_error;
  CVMetalTextureRef destination_ref = nullptr;
  void* destination_texture = nullptr;
  if (!resolve_destination_texture(destination_ref,
                                   destination_texture,
                                   destination_error)) {
    return fail_after_target_acquire(destination_error);
  }
  char ffi_error[256] = {};
  VPWgpuMetalRenderRequest request = {};
  request.destination_mtl_texture = destination_texture;
  request.output_format = output_target.ffi_output_format;
  request.output_color_mode = output_target.ffi_output_color_mode;
  request.sdr_white_scale = 1.0f;
  request.package_data = staging_buffer_.data();
  request.package_data_size = package.used_bytes;
  request.package = &package;
  request.overlay_fill_rects = overlay_primitives.fill_rects.empty()
      ? nullptr
      : overlay_primitives.fill_rects.data();
  request.overlay_fill_rect_count = overlay_primitives.fill_rects.size();
  request.overlay_line_rects = overlay_primitives.line_rects.empty()
      ? nullptr
      : overlay_primitives.line_rects.data();
  request.overlay_line_rect_count = overlay_primitives.line_rects.size();
  request.overlay_motion_lines = overlay_primitives.motion_lines.empty()
      ? nullptr
      : overlay_primitives.motion_lines.data();
  request.overlay_motion_line_count = overlay_primitives.motion_lines.size();
  request.overlay_generation = overlay_primitives.generation;
  request.width = draw_target_width_;
  request.height = draw_target_height_;
  apply_viewport_rect(request);
  request.error = ffi_error;
  request.error_size = sizeof(ffi_error);
  apply_external_flutter_surface(request);
  log_wgpu_decision("bgra-package",
                    package.decision,
                    viewport_rect_pixels,
                    package.storage,
                    output_target.ffi_output_format,
                    output_target.ffi_output_color_mode,
                    destination_texture);
  auto frame_info = frame_info_from_package(package, target);
  uint64_t source_generation = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    source_generation = ++retained_source_submitted_generation_;
  }
  auto pending = make_async_pending(frame_info,
                                    package_copy_us,
                                    package.storage,
                                    source_generation,
                                    source_metrics.signature,
                                    true);
  pending->destination_texture_ref = destination_ref;
  destination_ref = nullptr;
  pending->render_call_start = std::chrono::steady_clock::now();
  AsyncDrawPending* pending_raw = pending.release();
  VPWgpuMetalAsyncCompletion completion = {};
  completion.callback = wgpu_async_draw_completed;
  completion.user_data = pending_raw;
  completion.profiler_snapshot = &pending_raw->profiler_snapshot;
  pending_raw->has_profiler_snapshot = true;
  const int ret =
      VPWgpuMetalRendererRenderPackageAsync(wgpu_renderer_, &request, completion);
  if (ret != 0) {
    std::unique_ptr<AsyncDrawPending> reclaim(pending_raw);
    const std::string render_error =
        ffi_error[0] ? ffi_error : "wgpu-metal render package failed";
    return fail_after_target_acquire(render_error);
  }
  mark_external_flutter_surface_consumed();
  target_acquired = false;
  return true;
}

void WgpuMetalPresentationBackend::set_last_error(std::string error) {
  std::lock_guard<std::mutex> lock(mutex_);
  last_error_ = std::move(error);
}

void WgpuMetalPresentationBackend::mark_draw_failure(std::string error) {
  std::lock_guard<std::mutex> lock(mutex_);
  last_draw_succeeded_ = false;
  last_frame_info_available_ = false;
  ++draw_failure_count_;
  ++consecutive_draw_failures_;
  last_error_ = std::move(error);
}

void WgpuMetalPresentationBackend::mark_draw_success(
    const vr::PresentationBackendFrameInfo& frame_info,
    int32_t package_storage,
    uint64_t source_generation,
    uint64_t source_signature,
    bool source_upload) {
  std::lock_guard<std::mutex> lock(mutex_);
  last_draw_succeeded_ = true;
  last_frame_info_available_ = true;
  last_frame_info_ = frame_info;
  const bool commit_source =
      source_upload && source_generation != 0 &&
      source_generation == retained_source_submitted_generation_;
  if (commit_source) {
    retained_source_frame_info_available_ = true;
    retained_source_frame_info_ = frame_info;
    last_present_package_storage_ = package_storage;
    retained_source_available_ = true;
    retained_source_committed_generation_ = source_generation;
    last_source_signature_ = source_signature;
  }
  consecutive_draw_failures_ = 0;
  if (source_upload) {
    if (package_storage ==
        VPMacOSNativePresentPackageStorageSourceOutputAtlas) {
      ++cvpixelbuffer_upload_count_;
    }
    ++present_package_upload_count_;
  }
  last_error_.clear();
}

bool WgpuMetalPresentationBackend::target_installed_locked() const {
  if (draw_target_width_ <= 0 || draw_target_height_ <= 0) {
    return false;
  }
  if (!target_ring_enabled_) {
    return draw_target_pixel_buffer_ != nullptr;
  }
  return !target_ring_.empty();
}

WgpuMetalPresentationBackend::TargetAcquireResult
WgpuMetalPresentationBackend::acquire_draw_target_locked(
    const char* draw_source) {
  TargetAcquireResult result;
  const auto record_backpressure = [&](const char* reason,
                                       uint64_t limit,
                                       size_t target_count) {
    ++target_ring_backpressure_count_;
    last_error_ = reason;
    if (wgpu_profiler_enabled() &&
        (target_ring_backpressure_count_ <= 8 ||
         (target_ring_backpressure_count_ % 60) == 0)) {
      spdlog::info(
          "[WgpuMetalProfile] target_backpressure source={} in_flight={} "
          "limit={} targets={} count={} reason={}",
          draw_source ? draw_source : "",
          in_flight_draws_,
          limit,
          target_count,
          target_ring_backpressure_count_,
          reason);
    }
  };
  if (!target_ring_enabled_) {
    if (!draw_target_pixel_buffer_) {
      return result;
    }
    const uint64_t limit =
        vp_macos::kMetalPresentConcurrencyPolicy.max_single_target_in_flight;
    if (in_flight_draws_ >= limit) {
      record_backpressure(
          "renderer-owned wgpu-metal async draw deferred by backpressure",
          limit,
          draw_target_pixel_buffer_ ? 1u : 0u);
      return result;
    }
    ++in_flight_draws_;
    result.pixel_buffer = draw_target_pixel_buffer_;
    result.ring_generation = target_ring_generation_;
    return result;
  }
  const uint64_t limit =
      vp_macos::kMetalPresentConcurrencyPolicy.max_ring_in_flight;
  if (in_flight_draws_ >= limit) {
    record_backpressure(
        "renderer-owned wgpu-metal async draw deferred by backpressure",
        limit,
        target_ring_.size());
    return result;
  }
  const auto acquire_slot = [&](TargetSlot& slot) {
    slot.state = TargetState::InFlight;
    ++in_flight_draws_;
    result.pixel_buffer = slot.pixel_buffer;
    result.ring_generation = target_ring_generation_;
    result.slot_id = slot.slot_id;
  };
  for (auto& slot : target_ring_) {
    if (slot.state != TargetState::Available || !slot.pixel_buffer) {
      continue;
    }
    acquire_slot(slot);
    return result;
  }
  for (auto& slot : target_ring_) {
    if (slot.state != TargetState::Completed || !slot.pixel_buffer) {
      continue;
    }
    // Drop stale completed targets when Flutter has not consumed them quickly
    // enough during high-frequency viewport projection. Displayed/protected
    // targets have distinct states and are never reclaimed here.
    acquire_slot(slot);
    return result;
  }
  record_backpressure("renderer-owned wgpu-metal presentation target ring is busy",
                      limit,
                      target_ring_.size());
  return result;
}

void WgpuMetalPresentationBackend::complete_draw_target(
    uint64_t target_pixel_buffer_address,
    uint64_t target_ring_generation,
    uint64_t target_slot_id,
    bool success) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (target_pixel_buffer_address == 0) {
    return;
  }
  if (target_ring_generation != target_ring_generation_) {
    if (in_flight_draws_ > 0) {
      --in_flight_draws_;
    }
    return;
  }
  if (!target_ring_enabled_) {
    if (pointer_bits(draw_target_pixel_buffer_) == target_pixel_buffer_address &&
        in_flight_draws_ > 0) {
      --in_flight_draws_;
    }
    return;
  }
  for (auto& slot : target_ring_) {
    if (pointer_bits(slot.pixel_buffer) != target_pixel_buffer_address ||
        slot.slot_id != target_slot_id) {
      continue;
    }
    if (slot.state == TargetState::InFlight) {
      if (in_flight_draws_ > 0) {
        --in_flight_draws_;
      }
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

void* WgpuMetalPresentationBackend::capture_target_locked() const {
  if (last_frame_info_available_ &&
      last_frame_info_.target_pixel_buffer_address != 0) {
    void* last_target =
        pointer_from_bits(last_frame_info_.target_pixel_buffer_address);
    if (!target_ring_enabled_) {
      if (!draw_target_pixel_buffer_ || draw_target_pixel_buffer_ == last_target) {
        return last_target;
      }
    } else {
      for (const auto& slot : target_ring_) {
        if (slot.pixel_buffer == last_target) {
          return last_target;
        }
      }
    }
  }
  return current_draw_target_locked();
}

void* WgpuMetalPresentationBackend::current_draw_target_locked() const {
  if (!target_ring_enabled_) {
    return draw_target_pixel_buffer_;
  }
  for (const auto& slot : target_ring_) {
    if (slot.state == TargetState::Displayed && slot.pixel_buffer) {
      return slot.pixel_buffer;
    }
  }
  return target_ring_.empty() ? nullptr : target_ring_.front().pixel_buffer;
}

WgpuMetalPresentationBackend::SourceMetricsResult
WgpuMetalPresentationBackend::record_source_metrics(
    const vr::RendererDrawSnapshot& snapshot,
    const vr::PresentationBackendDrawHooks& hooks,
    int32_t /*target_width*/,
    int32_t /*target_height*/,
    int32_t /*track_slots*/,
    int32_t output_format,
    int32_t output_color_mode) {
  SourceMetricsResult result;
  const bool is_viewport_composite =
      hooks.draw_source && std::strcmp(hooks.draw_source, "viewport_composite") == 0;
  result.viewport_composite = is_viewport_composite;
  const uint64_t signature =
      source_frame_signature(snapshot, output_format, output_color_mode);
  result.signature = signature;
  std::lock_guard<std::mutex> lock(mutex_);
  if (is_viewport_composite) {
    ++viewport_composite_count_;
  }
  if (retained_source_available_ && last_source_signature_ != 0 &&
      last_source_signature_ == signature) {
    ++source_frame_cache_hit_count_;
    result.cache_hit = true;
    return result;
  }
  ++source_frame_cache_miss_count_;
  ++video_source_update_count_;
  return result;
}

void WgpuMetalPresentationBackend::record_wgpu_command_result(uint64_t elapsed_us,
                                                              bool success) {
  std::lock_guard<std::mutex> lock(mutex_);
  ++metal_command_completion_sample_count_;
  metal_command_completion_samples_us_.push_back(elapsed_us);
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
  if (!success) {
    ++metal_command_failure_count_;
  }
}

void WgpuMetalPresentationBackend::record_wgpu_phase_timings(
    uint64_t total_us,
    uint64_t pre_render_us,
    const VPWgpuMetalProfilerSnapshot* profiler) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto append_sample = [](std::vector<uint64_t>& samples, uint64_t value) {
    samples.push_back(value);
    if (samples.size() > 512) {
      samples.erase(samples.begin(),
                    samples.begin() +
                        static_cast<std::ptrdiff_t>(samples.size() - 512));
    }
  };
  append_sample(wgpu_compose_total_samples_us_, total_us);
  append_sample(wgpu_compose_pre_render_samples_us_, pre_render_us);
  if (profiler) {
    append_sample(wgpu_compose_import_samples_us_, profiler->last_import_us);
    append_sample(wgpu_compose_prepare_samples_us_, profiler->last_prepare_us);
    append_sample(wgpu_compose_overlay_encode_samples_us_,
                  profiler->last_overlay_encode_us);
    append_sample(wgpu_compose_bind_group_samples_us_,
                  profiler->last_bind_group_us);
    append_sample(wgpu_compose_pass_encode_samples_us_,
                  profiler->last_pass_encode_us);
    append_sample(wgpu_compose_submit_samples_us_, profiler->last_submit_us);
    append_sample(wgpu_compose_cpu_render_samples_us_,
                  profiler->last_cpu_render_us);
  }
  ++wgpu_phase_sample_count_;
  if (wgpu_phase_sample_count_ <= 16 || (wgpu_phase_sample_count_ % 16) == 0) {
    wgpu_compose_total_p95_us_ =
        percentile_95_us(wgpu_compose_total_samples_us_);
    wgpu_compose_pre_render_p95_us_ =
        percentile_95_us(wgpu_compose_pre_render_samples_us_);
    wgpu_compose_import_p95_us_ =
        percentile_95_us(wgpu_compose_import_samples_us_);
    wgpu_compose_prepare_p95_us_ =
        percentile_95_us(wgpu_compose_prepare_samples_us_);
    wgpu_compose_overlay_encode_p95_us_ =
        percentile_95_us(wgpu_compose_overlay_encode_samples_us_);
    wgpu_compose_bind_group_p95_us_ =
        percentile_95_us(wgpu_compose_bind_group_samples_us_);
    wgpu_compose_pass_encode_p95_us_ =
        percentile_95_us(wgpu_compose_pass_encode_samples_us_);
    wgpu_compose_submit_p95_us_ =
        percentile_95_us(wgpu_compose_submit_samples_us_);
    wgpu_compose_cpu_render_p95_us_ =
        percentile_95_us(wgpu_compose_cpu_render_samples_us_);
  }
}

void WgpuMetalPresentationBackend::record_present_package_timing(
    uint64_t copy_us,
    uint64_t gpu_wait_us,
    uint64_t total_us) {
  std::lock_guard<std::mutex> lock(mutex_);
  last_present_package_copy_us_ = copy_us;
  last_present_package_gpu_wait_us_ = gpu_wait_us;
  last_present_package_total_us_ = total_us;
}

void WgpuMetalPresentationBackend::complete_async_draw(
    std::unique_ptr<AsyncDrawPending> pending,
    bool success) {
  if (!pending) {
    return;
  }
  const uint64_t total_us = elapsed_us_since(pending->draw_start);
  const uint64_t pre_render_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          pending->render_call_start - pending->draw_start)
          .count());
  const uint64_t gpu_wait_us = elapsed_us_since(pending->render_call_start);
  record_wgpu_command_result(gpu_wait_us, success);
  record_wgpu_phase_timings(
      total_us,
      pre_render_us,
      pending->has_profiler_snapshot ? &pending->profiler_snapshot : nullptr);
  record_present_package_timing(pending->package_copy_us, gpu_wait_us, total_us);
  if (wgpu_profiler_enabled() && pending->has_profiler_snapshot) {
    const VPWgpuMetalProfilerSnapshot& profiler = pending->profiler_snapshot;
    spdlog::info(
        "[WgpuMetalProfile] total_us={} pre_render_us={} gpu_wait_us={} success={} "
        "dst_import={}/{} src_import={}/{} cache={} evict={} "
        "bind_groups(final={},overlay={}) overlay_layer(rebuild={},reuse={}) "
        "buffer_writes(package={},params={},overlay={}) submits={} "
        "phase(import={},prepare={},overlay={},bind={},pass={},submit={},cpu={},completion={})",
        total_us,
        pre_render_us,
        gpu_wait_us,
        success,
        profiler.destination_import_count,
        profiler.destination_import_reuse_count,
        profiler.source_import_count,
        profiler.source_import_reuse_count,
        profiler.imported_texture_cache_size,
        profiler.imported_texture_cache_eviction_count,
        profiler.final_bind_group_create_count,
        profiler.overlay_bind_group_create_count,
        profiler.overlay_layer_rebuild_count,
        profiler.overlay_layer_reuse_count,
        profiler.package_buffer_write_count,
        profiler.params_buffer_write_count,
        profiler.overlay_buffer_write_count,
        profiler.submit_count,
        profiler.last_import_us,
        profiler.last_prepare_us,
        profiler.last_overlay_encode_us,
        profiler.last_bind_group_us,
        profiler.last_pass_encode_us,
        profiler.last_submit_us,
        profiler.last_cpu_render_us,
        gpu_wait_us);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    overlay_last_expected_ = pending->overlay_expected;
    overlay_last_applied_ = success && pending->overlay_expected;
    overlay_last_fill_rect_count_ = pending->overlay_fill_rect_count;
    overlay_last_line_rect_count_ = pending->overlay_line_rect_count;
    if (pending->overlay_expected) {
      ++overlay_expected_count_;
      if (success) {
        ++overlay_applied_count_;
        ++overlay_gpu_success_count_;
      } else {
        ++overlay_missed_count_;
        ++overlay_gpu_failure_count_;
      }
    }
  }
  if (success) {
    mark_draw_success(pending->frame_info,
                      pending->package_storage,
                      pending->source_generation,
                      pending->source_signature,
                      pending->source_upload);
  } else {
    mark_draw_failure("wgpu-metal async GPU completion failed");
  }
  if (pending->target_acquired) {
    complete_draw_target(pending->target_pixel_buffer_address,
                         pending->target_ring_generation,
                         pending->target_slot_id,
                         success);
    pending->target_acquired = false;
  }
  if (pending->hooks.record_frame_copy_us) {
    pending->hooks.record_frame_copy_us(pending->package_copy_us);
  }
  if (pending->hooks.async_draw_completed) {
    const char* error = success ? nullptr : "wgpu-metal async GPU completion failed";
    pending->hooks.async_draw_completed(
        success, error, total_us, success ? &pending->frame_info : nullptr);
  }
}

std::unique_ptr<vr::PresentationBackend> create_wgpu_metal_presentation_backend() {
  return std::make_unique<WgpuMetalPresentationBackend>();
}

}  // namespace vp_macos
