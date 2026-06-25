#include "macos/wgpu/wgpu_metal_presentation_backend.h"

#include "macos/presentation/presentation_package_builder.h"
#include "macos/metal/metal_texture_wrapping.h"
#include "macos/wgpu/wgpu_ffi_bridge.h"
#include "renderer/overlay/analysis_overlay_primitives.h"
#include "renderer/overlay/analysis_overlay_renderer.h"
#include "renderer/render/presentation_package.h"

#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
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

struct WgpuOutputTargetDescriptor {
  MTLPixelFormat metal_pixel_format = MTLPixelFormatInvalid;
  int32_t ffi_output_format = 0;
  int32_t ffi_output_color_mode = 0;
  const char* render_target_format = "unknown";
  const char* render_color_space = "unknown";
  bool render_supported = false;
};

bool resolve_output_target_descriptor(void* target,
                                      int32_t expected_width,
                                      int32_t expected_height,
                                      WgpuOutputTargetDescriptor& descriptor,
                                      std::string& error) {
  auto* pixel_buffer = as_pixel_buffer(target);
  if (!pixel_buffer) {
    error = "wgpu-metal presentation target is unavailable";
    return false;
  }
  const int32_t actual_width =
      static_cast<int32_t>(CVPixelBufferGetWidth(pixel_buffer));
  const int32_t actual_height =
      static_cast<int32_t>(CVPixelBufferGetHeight(pixel_buffer));
  if (actual_width != expected_width || actual_height != expected_height) {
    error = "wgpu-metal target pixel buffer dimensions do not match the presentation surface";
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
      descriptor.ffi_output_color_mode = VP_WGPU_METAL_OUTPUT_COLOR_MODE_EDR;
      descriptor.render_target_format = "RGBA16Float";
      descriptor.render_color_space = "wgpu-metal-edr";
      descriptor.render_supported = true;
      return true;
    default:
      error = "wgpu-metal target pixel buffer format is unsupported";
      return false;
  }
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
  result.generation = package->cache_generation;
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
      result.fill_rects.push_back(rect);
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
    for (const auto& line : track.motion_lines) {
      VPMacOSNativeOverlayGpuRect rect = {};
      rect.rect_uv0 = vr::pack_overlay_uv16(
          line.x0, track.video_width, line.y0, track.video_height);
      rect.rect_uv1 = vr::pack_overlay_uv16(
          line.x1, track.video_width, line.y1, track.video_height);
      rect.color_bgra = pack_overlay_bgra(line.color);
      rect.track_idx = pack_overlay_track_payload(track.slot, track.line_alpha);
      result.motion_lines.push_back(rect);
    }
  }
  return result;
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
  backend->complete_async_draw(std::move(pending), result == 0);
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->active_callbacks > 0) {
    --state->active_callbacks;
  }
  state->cv.notify_all();
}

WgpuMetalPresentationBackend::~WgpuMetalPresentationBackend() {
  shutdown();
}

bool WgpuMetalPresentationBackend::initialize(const vr::PresentationBackendConfig& config) {
  shutdown();
  async_state_ = std::make_shared<AsyncState>();
  async_state_->backend = this;
  headless_ = config.headless;
  width_ = config.width;
  height_ = config.height;
  draw_target_max_track_slots_ = std::max(1, config.max_track_slots);

  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) {
    mark_draw_failure("wgpu-metal failed to create Metal device");
    return false;
  }
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
  if (!update_headless_output(config.output,
                              config.width,
                              config.height,
                              config.max_track_slots)) {
    return false;
  }
  if (!wgpu_ffi_available()) {
    mark_draw_failure("wgpu-metal Rust FFI is not linked");
    shutdown();
    return false;
  }
  char ffi_error[256] = {};
  wgpu_renderer_ = VPWgpuMetalRendererCreate(ffi_error, sizeof(ffi_error));
  if (!wgpu_renderer_) {
    mark_draw_failure(ffi_error[0] ? ffi_error : "wgpu-metal renderer create failed");
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
    async_state_->cv.wait(lock, [&] {
      return async_state_->active_callbacks == 0;
    });
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    draw_target_pixel_buffer_ = nullptr;
    target_ring_.clear();
    target_ring_enabled_ = false;
    displayed_target_address_ = 0;
    protected_target_address_ = 0;
  }
  if (texture_cache_) {
    CFRelease(texture_cache_);
    texture_cache_ = nullptr;
  }
  if (wgpu_renderer_) {
    VPWgpuMetalRendererDestroy(wgpu_renderer_);
    wgpu_renderer_ = nullptr;
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

bool WgpuMetalPresentationBackend::update_headless_output(void* output,
                                                          int width,
                                                          int height,
                                                          int max_track_slots) {
  if (!output || width <= 0 || height <= 0) {
    clear_headless_output();
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  target_ring_.clear();
  target_ring_enabled_ = false;
  displayed_target_address_ = 0;
  protected_target_address_ = 0;
  WgpuOutputTargetDescriptor descriptor;
  std::string target_error;
  if (resolve_output_target_descriptor(output,
                                       width,
                                       height,
                                       descriptor,
                                       target_error)) {
    draw_target_output_format_ = descriptor.ffi_output_format;
    draw_target_output_color_mode_ = descriptor.ffi_output_color_mode;
    draw_target_render_format_ = descriptor.render_target_format;
    draw_target_color_space_ = descriptor.render_color_space;
  } else {
    draw_target_output_format_ = 0;
    draw_target_output_color_mode_ = 0;
    draw_target_render_format_ = "unsupported";
    draw_target_color_space_ = "unsupported";
  }
  draw_target_pixel_buffer_ = output;
  width_ = width;
  height_ = height;
  draw_target_width_ = width;
  draw_target_height_ = height;
  draw_target_max_track_slots_ = std::max(1, max_track_slots);
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
  std::lock_guard<std::mutex> lock(mutex_);
  target_ring_.clear();
  target_ring_.reserve(pixel_buffer_count);
  displayed_target_address_ = pointer_bits(displayed_pixel_buffer);
  protected_target_address_ = pointer_bits(protected_pixel_buffer);
  WgpuOutputTargetDescriptor ring_descriptor;
  std::string ring_target_error;
  bool ring_descriptor_available = false;
  for (size_t i = 0; i < pixel_buffer_count; ++i) {
    if (pixel_buffers[i]) {
      void* pixel_buffer = const_cast<void*>(pixel_buffers[i]);
      if (!ring_descriptor_available) {
        ring_descriptor_available = resolve_output_target_descriptor(
            pixel_buffer,
            width,
            height,
            ring_descriptor,
            ring_target_error);
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
  }
  if (target_ring_.empty()) {
    return false;
  }
  target_ring_enabled_ = target_ring_.size() >= 2;
  draw_target_pixel_buffer_ = target_ring_enabled_ ? nullptr
                                                   : target_ring_.front().pixel_buffer;
  if (ring_descriptor_available) {
    draw_target_output_format_ = ring_descriptor.ffi_output_format;
    draw_target_output_color_mode_ = ring_descriptor.ffi_output_color_mode;
    draw_target_render_format_ = ring_descriptor.render_target_format;
    draw_target_color_space_ = ring_descriptor.render_color_space;
  } else {
    draw_target_output_format_ = 0;
    draw_target_output_color_mode_ = 0;
    draw_target_render_format_ = "unsupported";
    draw_target_color_space_ = "unsupported";
  }
  width_ = width;
  height_ = height;
  draw_target_width_ = width;
  draw_target_height_ = height;
  draw_target_max_track_slots_ = std::max(1, max_track_slots);
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
  draw_target_pixel_buffer_ = nullptr;
  target_ring_.clear();
  target_ring_enabled_ = false;
  displayed_target_address_ = 0;
  protected_target_address_ = 0;
  draw_target_width_ = 0;
  draw_target_height_ = 0;
  draw_target_output_format_ = 0;
  draw_target_output_color_mode_ = 0;
  draw_target_render_format_ = "unknown";
  draw_target_color_space_ = "unknown";
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
  if (target_ring_enabled_) {
    for (const auto& slot : target_ring_) {
      if (slot.state == TargetState::InFlight) {
        ++stats.in_flight_metal_buffer_count;
      }
    }
  }
  stats.metal_buffer_exhaustion_count = target_ring_backpressure_count_;
  stats.metal_command_completion_p95_us = metal_command_completion_p95_us_;
  stats.metal_command_failure_count = metal_command_failure_count_;
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
  diagnostics.source_cache_texture_count = retained_source_available_ ? 1 : 0;
  diagnostics.source_cache_publish_count = video_source_update_count_;
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
  {
    std::lock_guard<std::mutex> lock(mutex_);
    target = capture_target_locked();
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
  {
    std::lock_guard<std::mutex> lock(mutex_);
    target = capture_target_locked();
  }
  auto* pixel_buffer = as_pixel_buffer(target);
  if (!pixel_buffer ||
      CVPixelBufferLockBaseAddress(pixel_buffer, kCVPixelBufferLock_ReadOnly) !=
          kCVReturnSuccess) {
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
  void* target = nullptr;
  bool target_ring_acquired = false;
  bool target_ring_enabled = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    target_ring_enabled = target_ring_enabled_;
    target = acquire_draw_target_locked();
  }
  target_ring_acquired = target_ring_enabled && target;
  const uint64_t acquired_target_address = pointer_bits(target);
  auto complete_acquired_target = [&](bool success) {
    if (!target_ring_acquired || acquired_target_address == 0) {
      return;
    }
    complete_ring_draw_target(acquired_target_address, success);
    target_ring_acquired = false;
  };
  auto fail_after_target_acquire = [&](std::string error) {
    complete_acquired_target(false);
    mark_draw_failure(std::move(error));
    return false;
  };
  if (!target && target_ring_enabled) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++target_ring_backpressure_count_;
    }
    mark_draw_failure("renderer-owned wgpu-metal presentation target ring is busy");
    return false;
  }
  if (!metal_device_ || !texture_cache_ || !target ||
      draw_target_width_ <= 0 || draw_target_height_ <= 0) {
    return fail_after_target_acquire("wgpu-metal presentation target is unavailable");
  }
  WgpuOutputTargetDescriptor output_target;
  std::string target_error;
  if (!resolve_output_target_descriptor(target,
                                        draw_target_width_,
                                        draw_target_height_,
                                        output_target,
                                        target_error)) {
    return fail_after_target_acquire(target_error);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    draw_target_output_format_ = output_target.ffi_output_format;
    draw_target_output_color_mode_ = output_target.ffi_output_color_mode;
    draw_target_render_format_ = output_target.render_target_format;
    draw_target_color_space_ = output_target.render_color_space;
  }

  const int32_t track_slots = std::clamp(draw_target_max_track_slots_,
                                         1,
                                         static_cast<int>(VPMacOSNativeMaxTracks));
  const auto source_metrics = record_source_metrics(
      snapshot, hooks, draw_target_width_, draw_target_height_, track_slots);
  bool retained_source_available = false;
  int32_t retained_source_storage = 0;
  bool retained_source_frame_info_available = false;
  vr::PresentationBackendFrameInfo retained_source_frame_info;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    retained_source_available = retained_source_available_;
    retained_source_storage = last_present_package_storage_;
    retained_source_frame_info_available = retained_source_frame_info_available_;
    retained_source_frame_info = retained_source_frame_info_;
  }

  const auto overlay_primitives =
      build_overlay_primitives_for_wgpu(snapshot, hooks);
  const bool overlay_expected = overlay_primitives_expected(overlay_primitives);
  auto make_async_pending = [&](vr::PresentationBackendFrameInfo frame_info,
                                uint64_t package_copy_us,
                                int32_t package_storage,
                                bool source_upload) {
    auto pending = std::make_unique<AsyncDrawPending>();
    pending->state = async_state_;
    pending->hooks = hooks;
    pending->frame_info = frame_info;
    pending->draw_start = draw_start;
    pending->target_pixel_buffer_address = acquired_target_address;
    pending->package_copy_us = package_copy_us;
    pending->package_storage = package_storage;
    pending->source_upload = source_upload;
    pending->target_ring_acquired = target_ring_acquired;
    pending->overlay_expected = overlay_expected;
    pending->overlay_fill_rect_count = overlay_primitives.fill_rects.size();
    pending->overlay_line_rect_count = overlay_primitives.line_rects.size();
    return pending;
  };

  if (source_metrics.viewport_composite && source_metrics.cache_hit &&
      retained_source_available) {
    VPMacOSNativePresentDecisionInfo retained_decision = {};
    fill_present_decision_info_from_snapshot(snapshot,
                                             draw_target_width_,
                                             draw_target_height_,
                                             &retained_decision);
    CVMetalTextureRef destination_ref = nullptr;
    const CVReturn texture_status = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault,
        static_cast<CVMetalTextureCacheRef>(texture_cache_),
        as_pixel_buffer(target),
        nullptr,
        output_target.metal_pixel_format,
        draw_target_width_,
        draw_target_height_,
        0,
        &destination_ref);
    if (texture_status != kCVReturnSuccess || !destination_ref) {
      return fail_after_target_acquire(
          "wgpu-metal failed to wrap target CVPixelBuffer");
    }
    id<MTLTexture> destination_texture = CVMetalTextureGetTexture(destination_ref);
    char ffi_error[256] = {};
    VPWgpuMetalRetainedCompositeRequest request = {};
    request.destination_mtl_texture = (__bridge void*)destination_texture;
    request.output_format = output_target.ffi_output_format;
    request.output_color_mode = output_target.ffi_output_color_mode;
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
    request.error = ffi_error;
    request.error_size = sizeof(ffi_error);
    auto frame_info = retained_source_frame_info_available
        ? retained_source_frame_info
        : frame_info_from_decision(retained_decision, target);
    frame_info.target_pixel_buffer_address = pointer_bits(target);
    auto pending =
        make_async_pending(frame_info, 0, retained_source_storage, false);
    pending->destination_texture_ref = destination_ref;
    destination_ref = nullptr;
    AsyncDrawPending* pending_raw = pending.release();
    VPWgpuMetalAsyncCompletion completion = {};
    completion.callback = wgpu_async_draw_completed;
    completion.user_data = pending_raw;
    const int ret = VPWgpuMetalRendererCompositeRetainedSourceAsync(
        wgpu_renderer_, &request, completion);
    if (ret != 0) {
      std::unique_ptr<AsyncDrawPending> reclaim(pending_raw);
      const std::string error =
          ffi_error[0] ? ffi_error : "wgpu-metal retained composite failed";
      return fail_after_target_acquire(error);
    }
    target_ring_acquired = false;
    return true;
  }

  const auto storage_mix = snapshot_storage_mix(snapshot);
  VPMacOSNativeCVPixelBufferPresentFrameSet frame_set = {};
  std::string cv_error;
  if (snapshot_cv_pixel_buffer_frame_set(snapshot,
                                         draw_target_width_,
                                         draw_target_height_,
                                         &frame_set,
                                         cv_error)) {
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
      const MTLPixelFormat y_format =
          is_p010 ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
      const MTLPixelFormat uv_format =
          is_p010 ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;
      const CVReturn y_status = create_cv_metal_texture(
          static_cast<CVMetalTextureCacheRef>(texture_cache_),
          source_pixel_buffer,
          y_format,
          CVPixelBufferGetWidthOfPlane(source_pixel_buffer, 0),
          CVPixelBufferGetHeightOfPlane(source_pixel_buffer, 0),
          0,
          &source_y_refs[slot]);
      const CVReturn uv_status = create_cv_metal_texture(
          static_cast<CVMetalTextureCacheRef>(texture_cache_),
          source_pixel_buffer,
          uv_format,
          CVPixelBufferGetWidthOfPlane(source_pixel_buffer, 1),
          CVPixelBufferGetHeightOfPlane(source_pixel_buffer, 1),
          1,
          &source_uv_refs[slot]);
      if (y_status != kCVReturnSuccess || uv_status != kCVReturnSuccess ||
          !source_y_refs[slot].valid() || !source_uv_refs[slot].valid()) {
        return fail_after_target_acquire(
            "wgpu-metal failed to wrap source CVPixelBuffer planes");
      }
      request.source_y_mtl_textures[slot] =
          (__bridge void*)source_y_refs[slot].texture();
      request.source_uv_mtl_textures[slot] =
          (__bridge void*)source_uv_refs[slot].texture();
    }

    CVMetalTextureRef destination_ref = nullptr;
    const CVReturn texture_status = CVMetalTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault,
        static_cast<CVMetalTextureCacheRef>(texture_cache_),
        as_pixel_buffer(target),
        nullptr,
        output_target.metal_pixel_format,
        draw_target_width_,
        draw_target_height_,
        0,
        &destination_ref);
    if (texture_status != kCVReturnSuccess || !destination_ref) {
      return fail_after_target_acquire(
          "wgpu-metal failed to wrap target CVPixelBuffer");
    }
    id<MTLTexture> destination_texture = CVMetalTextureGetTexture(destination_ref);
    char ffi_error[256] = {};
    request.destination_mtl_texture = (__bridge void*)destination_texture;
    request.output_format = output_target.ffi_output_format;
    request.output_color_mode = output_target.ffi_output_color_mode;
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
    request.error = ffi_error;
    request.error_size = sizeof(ffi_error);
    const auto frame_info = frame_info_from_decision(frame_set.decision, target);
    auto pending = make_async_pending(
        frame_info, 0, VPMacOSNativePresentPackageStorageCVPixelBuffer, true);
    pending->destination_texture_ref = destination_ref;
    destination_ref = nullptr;
    for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
      if (source_y_refs[slot].get()) {
        pending->source_y_texture_refs[slot] =
            const_cast<void*>(CFRetain(source_y_refs[slot].get()));
      }
      if (source_uv_refs[slot].get()) {
        pending->source_uv_texture_refs[slot] =
            const_cast<void*>(CFRetain(source_uv_refs[slot].get()));
      }
    }
    AsyncDrawPending* pending_raw = pending.release();
    VPWgpuMetalAsyncCompletion completion = {};
    completion.callback = wgpu_async_draw_completed;
    completion.user_data = pending_raw;
    const int ret = VPWgpuMetalRendererRenderCVPixelBufferFrameSetAsync(
        wgpu_renderer_, &request, completion);
    if (ret != 0) {
      std::unique_ptr<AsyncDrawPending> reclaim(pending_raw);
      const std::string error =
          ffi_error[0] ? ffi_error : "wgpu-metal render CVPixelBuffer frame set failed";
      return fail_after_target_acquire(error);
    }
    target_ring_acquired = false;
    return true;
  }
  if (storage_mix.any_cv_pixel_buffer && !storage_mix.any_non_cv_pixel_buffer) {
    return fail_after_target_acquire(
        cv_error.empty() ? "wgpu-metal CVPixelBuffer frame set is invalid" : cv_error);
  }

  const auto package_layout = vr::describe_presentation_package_layout(
      draw_target_width_, draw_target_height_, track_slots);
  if (package_layout.max_bytes == 0 ||
      package_layout.bgra_row_bytes >
          static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    return fail_after_target_acquire(
        "wgpu-metal presentation package layout is invalid");
  }
  if (staging_buffer_.size() < package_layout.max_bytes) {
    staging_buffer_.assign(package_layout.max_bytes, 0);
    ++staging_allocation_count_;
    staging_max_bytes_ = std::max(staging_max_bytes_, staging_buffer_.size());
  } else {
    ++staging_reuse_count_;
  }
  VPMacOSNativePresentFramePackageInfo package = {};
  package.width = draw_target_width_;
  package.height = draw_target_height_;
  package.max_track_slots = track_slots;
  std::string error;
  const auto package_copy_start = std::chrono::steady_clock::now();
  if (copy_snapshot_yuv_package(snapshot,
                                staging_buffer_.data(),
                                staging_buffer_.size(),
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
                                    staging_buffer_.data(),
                                    staging_buffer_.size(),
                                    draw_target_width_,
                                    draw_target_height_,
                                    package.stride_bytes,
                                    package.track_stride_bytes,
                                    &package,
                                    error)) {
      return fail_after_target_acquire(error);
    }
    package.storage = VPMacOSNativePresentPackageStorageBGRA;
  }
  const uint64_t package_copy_us = elapsed_us_since(package_copy_start);
  CVMetalTextureRef destination_ref = nullptr;
  const CVReturn texture_status = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      static_cast<CVMetalTextureCacheRef>(texture_cache_),
      as_pixel_buffer(target),
      nullptr,
      output_target.metal_pixel_format,
      draw_target_width_,
      draw_target_height_,
      0,
      &destination_ref);
  if (texture_status != kCVReturnSuccess || !destination_ref) {
    return fail_after_target_acquire(
        "wgpu-metal failed to wrap target CVPixelBuffer");
  }
  id<MTLTexture> destination_texture = CVMetalTextureGetTexture(destination_ref);
  char ffi_error[256] = {};
  VPWgpuMetalRenderRequest request = {};
  request.destination_mtl_texture = (__bridge void*)destination_texture;
  request.output_format = output_target.ffi_output_format;
  request.output_color_mode = output_target.ffi_output_color_mode;
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
  request.error = ffi_error;
  request.error_size = sizeof(ffi_error);
  auto frame_info = frame_info_from_package(package, target);
  auto pending = make_async_pending(frame_info, package_copy_us, package.storage, true);
  pending->destination_texture_ref = destination_ref;
  destination_ref = nullptr;
  AsyncDrawPending* pending_raw = pending.release();
  VPWgpuMetalAsyncCompletion completion = {};
  completion.callback = wgpu_async_draw_completed;
  completion.user_data = pending_raw;
  const int ret =
      VPWgpuMetalRendererRenderPackageAsync(wgpu_renderer_, &request, completion);
  if (ret != 0) {
    std::unique_ptr<AsyncDrawPending> reclaim(pending_raw);
    const std::string render_error =
        ffi_error[0] ? ffi_error : "wgpu-metal render package failed";
    return fail_after_target_acquire(render_error);
  }
  target_ring_acquired = false;
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
  retained_source_available_ = false;
  retained_source_frame_info_available_ = false;
  ++draw_failure_count_;
  ++consecutive_draw_failures_;
  last_error_ = std::move(error);
}

void WgpuMetalPresentationBackend::mark_draw_success(
    const vr::PresentationBackendFrameInfo& frame_info,
    int32_t package_storage,
    bool source_upload) {
  std::lock_guard<std::mutex> lock(mutex_);
  last_draw_succeeded_ = true;
  last_frame_info_available_ = true;
  last_frame_info_ = frame_info;
  if (source_upload) {
    retained_source_frame_info_available_ = true;
    retained_source_frame_info_ = frame_info;
  }
  consecutive_draw_failures_ = 0;
  last_present_package_storage_ = package_storage;
  retained_source_available_ = true;
  if (source_upload) {
    if (package_storage == VPMacOSNativePresentPackageStorageCVPixelBuffer) {
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

void* WgpuMetalPresentationBackend::acquire_draw_target_locked() {
  if (!target_ring_enabled_) {
    return draw_target_pixel_buffer_;
  }
  for (auto& slot : target_ring_) {
    if (slot.state != TargetState::Available || !slot.pixel_buffer) {
      continue;
    }
    slot.state = TargetState::InFlight;
    return slot.pixel_buffer;
  }
  return nullptr;
}

void WgpuMetalPresentationBackend::complete_ring_draw_target(
    uint64_t target_pixel_buffer_address,
    bool success) {
  std::lock_guard<std::mutex> lock(mutex_);
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
    int32_t target_width,
    int32_t target_height,
    int32_t track_slots) {
  SourceMetricsResult result;
  const bool is_viewport_composite =
      hooks.draw_source && std::strcmp(hooks.draw_source, "viewport_composite") == 0;
  result.viewport_composite = is_viewport_composite;
  const uint64_t signature =
      source_frame_signature(snapshot, target_width, target_height, track_slots);
  std::lock_guard<std::mutex> lock(mutex_);
  if (is_viewport_composite) {
    ++viewport_composite_count_;
  }
  if (last_source_signature_ != 0 && last_source_signature_ == signature) {
    ++source_frame_cache_hit_count_;
    result.cache_hit = true;
    return result;
  }
  ++source_frame_cache_miss_count_;
  ++video_source_update_count_;
  last_source_signature_ = signature;
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
  const uint64_t gpu_wait_us =
      total_us > pending->package_copy_us ? total_us - pending->package_copy_us : total_us;
  record_wgpu_command_result(gpu_wait_us, success);
  record_present_package_timing(pending->package_copy_us, gpu_wait_us, total_us);
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
                      pending->source_upload);
  } else {
    mark_draw_failure("wgpu-metal async GPU completion failed");
  }
  if (pending->target_ring_acquired) {
    complete_ring_draw_target(pending->target_pixel_buffer_address, success);
    pending->target_ring_acquired = false;
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
