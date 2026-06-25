#include "macos/metal/metal_presentation_backend.h"

#include "macos/wgpu/wgpu_ffi_bridge.h"
#include "renderer/decode/yuv_to_bgra.h"
#include "renderer/overlay/analysis_overlay_primitives.h"
#include "renderer/render/presentation_backend_factory.h"
#include "renderer/render/renderer_draw_snapshot.h"

#include <CoreVideo/CoreVideo.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

int fail(const char* message) {
  std::cerr << message << "\n";
  return 1;
}

bool draw_wgpu_frame_and_wait(vr::PresentationBackend& backend,
                              const vr::RendererDrawSnapshot& snapshot,
                              vr::PresentationBackendDrawHooks hooks = {},
                              std::string* error = nullptr) {
  std::mutex mutex;
  std::condition_variable cv;
  bool completed = false;
  bool success = false;
  std::string completion_error;
  auto user_async_completed = std::move(hooks.async_draw_completed);
  hooks.async_draw_completed =
      [&](bool draw_success,
          const char* draw_error,
          uint64_t backend_us,
          const vr::PresentationBackendFrameInfo* frame_info) mutable {
        if (user_async_completed) {
          user_async_completed(draw_success, draw_error, backend_us, frame_info);
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          completed = true;
          success = draw_success;
          completion_error = draw_error ? draw_error : "";
        }
        cv.notify_one();
      };
  if (!backend.draw_frame(snapshot, hooks)) {
    if (error) {
      *error = backend.last_error();
    }
    return false;
  }
  std::unique_lock<std::mutex> lock(mutex);
  if (!cv.wait_for(lock, std::chrono::seconds(5), [&] { return completed; })) {
    if (error) {
      *error = "wgpu-metal async draw timed out";
    }
    return false;
  }
  if (!success && error) {
    *error = completion_error;
  }
  return success;
}

struct Bgra {
  uint8_t b = 0;
  uint8_t g = 0;
  uint8_t r = 0;
  uint8_t a = 255;
};

struct ScopedCVPixelBuffer {
  CVPixelBufferRef buffer = nullptr;

  ~ScopedCVPixelBuffer() {
    if (buffer) {
      CVPixelBufferRelease(buffer);
    }
  }

  ScopedCVPixelBuffer(const ScopedCVPixelBuffer&) = delete;
  ScopedCVPixelBuffer& operator=(const ScopedCVPixelBuffer&) = delete;

  ScopedCVPixelBuffer() = default;
  ScopedCVPixelBuffer(ScopedCVPixelBuffer&& other) noexcept
      : buffer(other.buffer) {
    other.buffer = nullptr;
  }
  ScopedCVPixelBuffer& operator=(ScopedCVPixelBuffer&& other) noexcept {
    if (this != &other) {
      if (buffer) {
        CVPixelBufferRelease(buffer);
      }
      buffer = other.buffer;
      other.buffer = nullptr;
    }
    return *this;
  }
};

float half_to_float(uint16_t half) {
  const double sign = (half & 0x8000u) != 0 ? -1.0 : 1.0;
  const int exponent = static_cast<int>((half >> 10u) & 0x1fu);
  const int mantissa = static_cast<int>(half & 0x03ffu);
  if (exponent == 0) {
    if (mantissa == 0) {
      return static_cast<float>(sign * 0.0);
    }
    return static_cast<float>(
        sign * std::ldexp(static_cast<double>(mantissa), -24));
  }
  if (exponent == 0x1fu) {
    if (mantissa == 0) {
      return static_cast<float>(sign * std::numeric_limits<double>::infinity());
    }
    return std::numeric_limits<float>::quiet_NaN();
  }
  const double value = 1.0 + static_cast<double>(mantissa) / 1024.0;
  return static_cast<float>(sign * std::ldexp(value, exponent - 15));
}

bool measure_rgba_half_edr(CVPixelBufferRef buffer,
                           float* max_rgb,
                           int* pixels_over_one,
                           int* sample_count) {
  if (!max_rgb || !pixels_over_one || !sample_count) {
    return false;
  }
  *max_rgb = 0.0f;
  *pixels_over_one = 0;
  *sample_count = 0;
  if (!buffer ||
      CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_64RGBAHalf ||
      CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) !=
          kCVReturnSuccess) {
    return false;
  }
  const auto* base =
      static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  const int width = static_cast<int>(CVPixelBufferGetWidth(buffer));
  const int height = static_cast<int>(CVPixelBufferGetHeight(buffer));
  if (!base || width <= 0 || height <= 0) {
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return false;
  }
  for (int y = 0; y < height; ++y) {
    const auto* row = base + static_cast<size_t>(y) * stride;
    for (int x = 0; x < width; ++x) {
      const auto* pixel =
          reinterpret_cast<const uint16_t*>(row + static_cast<size_t>(x) * 8u);
      const float r = half_to_float(pixel[0]);
      const float g = half_to_float(pixel[1]);
      const float b = half_to_float(pixel[2]);
      const float a = half_to_float(pixel[3]);
      const float pixel_max = std::max({r, g, b});
      if (!std::isfinite(pixel_max) || !std::isfinite(a)) {
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        return false;
      }
      *max_rgb = std::max(*max_rgb, pixel_max);
      if (pixel_max > 1.0f) {
        ++(*pixels_over_one);
      }
      if (std::abs(a - 1.0f) > 0.01f) {
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        return false;
      }
      ++(*sample_count);
    }
  }
  CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  return true;
}

void write_p010_sample_to_bytes(uint8_t* dst, uint16_t code) {
  if (!dst) {
    return;
  }
  const uint16_t raw = static_cast<uint16_t>(code << 6);
  dst[0] = static_cast<uint8_t>(raw & 0xffu);
  dst[1] = static_cast<uint8_t>((raw >> 8) & 0xffu);
}

ScopedCVPixelBuffer make_nv12_pixel_buffer(int width,
                                           int height,
                                           uint8_t y_value,
                                           uint8_t u_value,
                                           uint8_t v_value) {
  ScopedCVPixelBuffer holder;
  CFDictionaryRef io_surface_properties = CFDictionaryCreate(
      kCFAllocatorDefault,
      nullptr,
      nullptr,
      0,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  const void* keys[] = {
      kCVPixelBufferMetalCompatibilityKey,
      kCVPixelBufferIOSurfacePropertiesKey,
  };
  const void* values[] = {
      kCFBooleanTrue,
      io_surface_properties,
  };
  CFDictionaryRef attrs = CFDictionaryCreate(
      kCFAllocatorDefault,
      keys,
      values,
      2,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CVPixelBufferCreate(kCFAllocatorDefault,
                      width,
                      height,
                      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                      attrs,
                      &holder.buffer);
  if (attrs) {
    CFRelease(attrs);
  }
  if (io_surface_properties) {
    CFRelease(io_surface_properties);
  }
  if (!holder.buffer ||
      CVPixelBufferLockBaseAddress(holder.buffer, 0) != kCVReturnSuccess) {
    return holder;
  }
  auto* y_plane = static_cast<uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(holder.buffer, 0));
  auto* uv_plane = static_cast<uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(holder.buffer, 1));
  const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(holder.buffer, 0);
  const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(holder.buffer, 1);
  const int uv_height = (height + 1) / 2;
  const int uv_width = (width + 1) / 2;
  if (y_plane && uv_plane) {
    for (int y = 0; y < height; ++y) {
      std::fill_n(y_plane + static_cast<size_t>(y) * y_stride,
                  width,
                  y_value);
    }
    for (int y = 0; y < uv_height; ++y) {
      uint8_t* row = uv_plane + static_cast<size_t>(y) * uv_stride;
      for (int x = 0; x < uv_width; ++x) {
        row[x * 2 + 0] = u_value;
        row[x * 2 + 1] = v_value;
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(holder.buffer, 0);
  return holder;
}

ScopedCVPixelBuffer make_bgra_pixel_buffer(int width, int height) {
  ScopedCVPixelBuffer holder;
  CFDictionaryRef io_surface_properties = CFDictionaryCreate(
      kCFAllocatorDefault,
      nullptr,
      nullptr,
      0,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  const void* keys[] = {
      kCVPixelBufferMetalCompatibilityKey,
      kCVPixelBufferIOSurfacePropertiesKey,
  };
  const void* values[] = {
      kCFBooleanTrue,
      io_surface_properties,
  };
  CFDictionaryRef attrs = CFDictionaryCreate(
      kCFAllocatorDefault,
      keys,
      values,
      2,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CVPixelBufferCreate(kCFAllocatorDefault,
                      width,
                      height,
                      kCVPixelFormatType_32BGRA,
                      attrs,
                      &holder.buffer);
  if (attrs) {
    CFRelease(attrs);
  }
  if (io_surface_properties) {
    CFRelease(io_surface_properties);
  }
  return holder;
}

ScopedCVPixelBuffer make_rgba_half_pixel_buffer(int width, int height) {
  ScopedCVPixelBuffer holder;
  CFDictionaryRef io_surface_properties = CFDictionaryCreate(
      kCFAllocatorDefault,
      nullptr,
      nullptr,
      0,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  const void* keys[] = {
      kCVPixelBufferMetalCompatibilityKey,
      kCVPixelBufferIOSurfacePropertiesKey,
  };
  const void* values[] = {
      kCFBooleanTrue,
      io_surface_properties,
  };
  CFDictionaryRef attrs = CFDictionaryCreate(
      kCFAllocatorDefault,
      keys,
      values,
      2,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CVPixelBufferCreate(kCFAllocatorDefault,
                      width,
                      height,
                      kCVPixelFormatType_64RGBAHalf,
                      attrs,
                      &holder.buffer);
  if (attrs) {
    CFRelease(attrs);
  }
  if (io_surface_properties) {
    CFRelease(io_surface_properties);
  }
  return holder;
}

ScopedCVPixelBuffer make_p010_pixel_buffer(int width,
                                           int height,
                                           const uint16_t* y_codes,
                                           const uint16_t* uv_codes) {
  ScopedCVPixelBuffer holder;
  CFDictionaryRef io_surface_properties = CFDictionaryCreate(
      kCFAllocatorDefault,
      nullptr,
      nullptr,
      0,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  const void* keys[] = {
      kCVPixelBufferMetalCompatibilityKey,
      kCVPixelBufferIOSurfacePropertiesKey,
  };
  const void* values[] = {
      kCFBooleanTrue,
      io_surface_properties,
  };
  CFDictionaryRef attrs = CFDictionaryCreate(
      kCFAllocatorDefault,
      keys,
      values,
      2,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CVPixelBufferCreate(kCFAllocatorDefault,
                      width,
                      height,
                      kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
                      attrs,
                      &holder.buffer);
  if (attrs) {
    CFRelease(attrs);
  }
  if (io_surface_properties) {
    CFRelease(io_surface_properties);
  }
  if (!holder.buffer ||
      CVPixelBufferLockBaseAddress(holder.buffer, 0) != kCVReturnSuccess) {
    return holder;
  }
  auto* y_plane = static_cast<uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(holder.buffer, 0));
  auto* uv_plane = static_cast<uint8_t*>(
      CVPixelBufferGetBaseAddressOfPlane(holder.buffer, 1));
  const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(holder.buffer, 0);
  const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(holder.buffer, 1);
  const int uv_height = (height + 1) / 2;
  const int uv_width = (width + 1) / 2;
  if (y_plane && uv_plane && y_codes && uv_codes) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        write_p010_sample_to_bytes(
            y_plane + static_cast<size_t>(y) * y_stride +
                static_cast<size_t>(x) * 2u,
            y_codes[y * width + x]);
      }
    }
    for (int y = 0; y < uv_height; ++y) {
      for (int x = 0; x < uv_width; ++x) {
        const int block = y * uv_width + x;
        uint8_t* sample =
            uv_plane + static_cast<size_t>(y) * uv_stride +
            static_cast<size_t>(x) * 4u;
        write_p010_sample_to_bytes(sample, uv_codes[block * 2]);
        write_p010_sample_to_bytes(sample + 2, uv_codes[block * 2 + 1]);
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(holder.buffer, 0);
  return holder;
}

std::shared_ptr<void> retain_pixel_buffer(CVPixelBufferRef pixel_buffer) {
  if (!pixel_buffer) {
    return {};
  }
  CVPixelBufferRetain(pixel_buffer);
  return std::shared_ptr<void>(pixel_buffer, [](void* p) {
    if (p) {
      CVPixelBufferRelease(static_cast<CVPixelBufferRef>(p));
    }
  });
}

void write_p010_sample(std::vector<uint8_t>& data, size_t offset, uint16_t code) {
  if (offset + 1 >= data.size()) {
    return;
  }
  write_p010_sample_to_bytes(data.data() + offset, code);
}

Bgra reference_yuv_to_bgra(uint8_t y,
                           uint8_t u,
                           uint8_t v,
                           int color_range,
                           int color_matrix) {
  Bgra out;
  vr::write_yuv_to_bgra(y,
                        u,
                        v,
                        color_range,
                        color_matrix,
                        reinterpret_cast<uint8_t*>(&out));
  return out;
}

Bgra read_bgra(const std::vector<uint8_t>& data, int width, int x, int y) {
  const size_t offset =
      (static_cast<size_t>(y) * static_cast<size_t>(width) +
       static_cast<size_t>(x)) *
      4u;
  return {data[offset], data[offset + 1], data[offset + 2], data[offset + 3]};
}

bool expect_bgra_near(const char* name,
                      const std::vector<uint8_t>& data,
                      int width,
                      int x,
                      int y,
                      Bgra expected,
                      int tolerance) {
  const Bgra actual = read_bgra(data, width, x, y);
  const int diffs[] = {
      std::abs(static_cast<int>(actual.b) - expected.b),
      std::abs(static_cast<int>(actual.g) - expected.g),
      std::abs(static_cast<int>(actual.r) - expected.r),
      std::abs(static_cast<int>(actual.a) - expected.a),
  };
  if (diffs[0] <= tolerance && diffs[1] <= tolerance &&
      diffs[2] <= tolerance && diffs[3] <= tolerance) {
    return true;
  }
  std::cerr << name << " pixel mismatch at (" << x << "," << y
            << "): actual=(" << static_cast<int>(actual.b) << ","
            << static_cast<int>(actual.g) << ","
            << static_cast<int>(actual.r) << ","
            << static_cast<int>(actual.a) << ") expected=("
            << static_cast<int>(expected.b) << ","
            << static_cast<int>(expected.g) << ","
            << static_cast<int>(expected.r) << ","
            << static_cast<int>(expected.a) << ") tolerance="
            << tolerance << "\n";
  return false;
}

vr::TextureFrame make_cv_nv12_frame(CVPixelBufferRef pixel_buffer,
                                    int width,
                                    int height,
                                    int64_t pts_us) {
  vr::TextureFrame frame;
  frame.width = width;
  frame.height = height;
  frame.pts_us = pts_us;
  frame.duration_us = 33333;
  frame.is_nv12 = true;
  frame.color = {vr::VIDEO_COLOR_RANGE_LIMITED,
                 vr::VIDEO_COLOR_MATRIX_BT709,
                 vr::VIDEO_COLOR_TRANSFER_SDR,
                 vr::VIDEO_COLOR_PRIMARIES_BT709};
  frame.storage = vr::MacOSCVPixelBufferFrameStorage{
      pixel_buffer,
      kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
      2,
      false,
      width,
      height,
      retain_pixel_buffer(pixel_buffer),
  };
  return frame;
}

vr::TextureFrame make_cv_p010_frame(CVPixelBufferRef pixel_buffer,
                                    int width,
                                    int height,
                                    int64_t pts_us) {
  vr::TextureFrame frame;
  frame.width = width;
  frame.height = height;
  frame.pts_us = pts_us;
  frame.duration_us = 33333;
  frame.is_p010 = true;
  frame.color = {vr::VIDEO_COLOR_RANGE_LIMITED,
                 vr::VIDEO_COLOR_MATRIX_BT709,
                 vr::VIDEO_COLOR_TRANSFER_SDR,
                 vr::VIDEO_COLOR_PRIMARIES_BT709};
  frame.storage = vr::MacOSCVPixelBufferFrameStorage{
      pixel_buffer,
      kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
      2,
      true,
      width,
      height,
      retain_pixel_buffer(pixel_buffer),
  };
  return frame;
}

}  // namespace

int main() {
  constexpr int kWidth = 4;
  constexpr int kHeight = 4;
  CVPixelBufferRef pixel_buffer = nullptr;
  CFDictionaryRef io_surface_properties = CFDictionaryCreate(
      kCFAllocatorDefault,
      nullptr,
      nullptr,
      0,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  const void* keys[] = {
      kCVPixelBufferMetalCompatibilityKey,
      kCVPixelBufferIOSurfacePropertiesKey,
  };
  const void* values[] = {
      kCFBooleanTrue,
      io_surface_properties,
  };
  CFDictionaryRef attrs = CFDictionaryCreate(
      kCFAllocatorDefault,
      keys,
      values,
      2,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  if (CVPixelBufferCreate(kCFAllocatorDefault,
                          kWidth,
                          kHeight,
                          kCVPixelFormatType_32BGRA,
                          attrs,
                          &pixel_buffer) != kCVReturnSuccess ||
      !pixel_buffer) {
    if (attrs) {
      CFRelease(attrs);
    }
    if (io_surface_properties) {
      CFRelease(io_surface_properties);
    }
    return fail("Metal presentation backend draw smoke could not create pixel buffer");
  }
  if (attrs) {
    CFRelease(attrs);
  }
  if (io_surface_properties) {
    CFRelease(io_surface_properties);
  }

  vp_macos::MetalPresentationBackend backend;
  auto factory_backend =
      vr::create_presentation_backend(vr::RenderBackendKind::Metal);
  const auto* default_provider = vr::default_presentation_backend_provider();
  if (!default_provider ||
      !default_provider->supports(vr::RenderBackendKind::Metal) ||
      !default_provider->supports(vr::RenderBackendKind::WgpuMetal) ||
      default_provider->supports(vr::RenderBackendKind::D3D11)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend default provider support set is wrong");
  }
  auto provider_backend = default_provider->create(vr::RenderBackendKind::Metal);
  if (!provider_backend ||
      provider_backend->kind() != vr::PresentationBackendKind::Metal) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend provider did not create Metal backend");
  }
  if (!factory_backend ||
      factory_backend->kind() != vr::PresentationBackendKind::Metal ||
      std::string(factory_backend->name()) != "metal-cvpixelbuffer") {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend factory did not create Metal backend");
  }
  if (vr::create_presentation_backend(vr::RenderBackendKind::D3D11)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend factory created unsupported D3D11 backend");
  }
  auto wgpu_backend =
      vr::create_presentation_backend(vr::RenderBackendKind::WgpuMetal);
  if (!wgpu_backend ||
      wgpu_backend->kind() != vr::PresentationBackendKind::WgpuMetal ||
      std::string(wgpu_backend->name()) != "wgpu-metal") {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend factory did not create WgpuMetal backend");
  }
  vr::PresentationBackendConfig config;
  config.output = pixel_buffer;
  config.width = kWidth;
  config.height = kHeight;
  config.max_track_slots = 3;
  config.headless = true;
  if (!wgpu_backend->initialize(config)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not initialize target state");
  }
  const auto wgpu_initial_diagnostics = wgpu_backend->diagnostics();
  if (wgpu_initial_diagnostics.adapter_description.empty() ||
      wgpu_initial_diagnostics.adapter_description == "unknown" ||
      wgpu_initial_diagnostics.driver_type.find("Metal") == std::string::npos ||
      wgpu_initial_diagnostics.driver_type.find("texture-format-16bit-norm") ==
          std::string::npos ||
      wgpu_initial_diagnostics.feature_level != VP_WGPU_FFI_ABI_VERSION) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend adapter diagnostics did not initialize");
  }
  if (wgpu_backend->draw_frame(vr::RendererDrawSnapshot{},
                               vr::PresentationBackendDrawHooks{})) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend drew successfully before import is enabled");
  }
  const std::string wgpu_error = wgpu_backend->last_error();
  if (wgpu_error.find("Rust FFI") == std::string::npos &&
      wgpu_error.find("wgpu-hal") == std::string::npos &&
      wgpu_error.find("presentable frame") == std::string::npos) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not fail closed with wgpu diagnostics");
  }
  wgpu_backend->shutdown();

  if (!backend.initialize(config)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend did not initialize");
  }
  if (backend.kind() != vr::PresentationBackendKind::Metal) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend kind mismatch");
  }
  if (!backend.headless()) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend should be headless");
  }
  if (backend.width() != kWidth || backend.height() != kHeight) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend dimensions mismatch");
  }
  if (!backend.available() || !backend.uploader()) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend uploader is unavailable");
  }

  auto bgra = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(kWidth * kHeight * 4), 0);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    (*bgra)[static_cast<size_t>(i) * 4 + 0] = 16;
    (*bgra)[static_cast<size_t>(i) * 4 + 1] = 96;
    (*bgra)[static_cast<size_t>(i) * 4 + 2] = 192;
    (*bgra)[static_cast<size_t>(i) * 4 + 3] = 255;
  }
  (*bgra)[4] = 240;
  (*bgra)[5] = 32;
  (*bgra)[6] = 64;
  (*bgra)[7] = 255;
  vr::TextureFrame frame;
  frame.width = kWidth;
  frame.height = kHeight;
  frame.pts_us = 123000;
  frame.duration_us = 33333;
  frame.storage = vr::CpuRgbaFrameStorage{bgra, kWidth * 4};

  vr::RendererDrawSnapshot snapshot;
  snapshot.decision.should_present = true;
  snapshot.decision.current_pts_us = frame.pts_us;
  snapshot.decision.frames[0] = frame;
  snapshot.decision.file_ids[0] = 7;
  snapshot.decision.track_generations[0] = 1;
  snapshot.tracks[0].active = true;
  snapshot.tracks[0].file_id = 7;
  snapshot.tracks[0].generation = 1;
  snapshot.tracks[0].video_width = kWidth;
  snapshot.tracks[0].video_height = kHeight;
  snapshot.tracks[0].video_aspect = 1.0f;
  snapshot.track_geometry[0] = {true, kWidth, kHeight, 1.0f};
  snapshot.target_width = kWidth;
  snapshot.target_height = kHeight;

  uint64_t copy_metric_count = 0;
  vr::PresentationBackendDrawHooks hooks;
  hooks.record_frame_copy_us = [&copy_metric_count](uint64_t) {
    ++copy_metric_count;
  };
  if (!wgpu_backend->initialize(config)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not reinitialize for BGRA draw");
  }
  if (!wgpu_backend->completes_draw_asynchronously()) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not opt into async completion semantics");
  }
  auto wgpu_mismatch_target = make_bgra_pixel_buffer(kWidth - 1, kHeight);
  if (!wgpu_mismatch_target.buffer ||
      !wgpu_backend->update_headless_output(wgpu_mismatch_target.buffer,
                                            kWidth,
                                            kHeight,
                                            3)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend could not install mismatched target smoke");
  }
  if (wgpu_backend->draw_frame(snapshot, vr::PresentationBackendDrawHooks{})) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend accepted mismatched target dimensions");
  }
  if (std::string(wgpu_backend->last_error()).find("dimensions do not match") ==
      std::string::npos) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend mismatched target failure was not diagnostic");
  }
  auto wgpu_wrong_format_target =
      make_nv12_pixel_buffer(kWidth, kHeight, 96, 128, 128);
  if (!wgpu_wrong_format_target.buffer ||
      !wgpu_backend->update_headless_output(wgpu_wrong_format_target.buffer,
                                            kWidth,
                                            kHeight,
                                            3)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend could not install wrong-format target smoke");
  }
  if (wgpu_backend->draw_frame(snapshot, vr::PresentationBackendDrawHooks{})) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend accepted unsupported target format");
  }
  if (std::string(wgpu_backend->last_error()).find("format is unsupported") ==
      std::string::npos) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend wrong-format target failure was not diagnostic");
  }
  auto wgpu_edr_target = make_rgba_half_pixel_buffer(kWidth, kHeight);
  if (!wgpu_edr_target.buffer ||
      !wgpu_backend->update_headless_output(wgpu_edr_target.buffer,
                                            kWidth,
                                            kHeight,
                                            3)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend could not install RGBA16Float target smoke");
  }
  const auto edr_diagnostics = wgpu_backend->diagnostics();
  if (std::string(edr_diagnostics.render_target_format).find("RGBA16Float") ==
          std::string::npos ||
      std::string(edr_diagnostics.render_color_space).find("edr") ==
          std::string::npos) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not expose RGBA16Float EDR target diagnostics");
  }
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected RGBA16Float EDR target: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  if (wgpu_backend->presentation_stats().last_draw_succeeded == 0) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend RGBA16Float EDR draw did not report success");
  }
  if (!wgpu_backend->update_headless_output(pixel_buffer, kWidth, kHeight, 3)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend could not restore BGRA target");
  }
  int async_completion_count = 0;
  bool async_completion_success = false;
  std::string async_completion_error;
  uint64_t async_completion_backend_us = 0;
  uint64_t wgpu_copy_metric_count = 0;
  vr::PresentationBackendFrameInfo async_completion_frame_info = {};
  vr::PresentationBackendDrawHooks wgpu_async_hooks;
  wgpu_async_hooks.record_frame_copy_us = [&wgpu_copy_metric_count](uint64_t) {
    ++wgpu_copy_metric_count;
  };
  wgpu_async_hooks.async_draw_completed =
      [&](bool success,
          const char* error,
          uint64_t backend_us,
          const vr::PresentationBackendFrameInfo* frame_info) {
        ++async_completion_count;
        async_completion_success = success;
        async_completion_error = error ? error : "";
        async_completion_backend_us = backend_us;
        if (frame_info) {
          async_completion_frame_info = *frame_info;
        }
      };
  const auto wgpu_source_stats_base = wgpu_backend->presentation_stats();
  if (!draw_wgpu_frame_and_wait(*wgpu_backend, snapshot, wgpu_async_hooks)) {
    std::cerr << "WgpuMetal backend rejected BGRA snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  if (async_completion_count != 1 || !async_completion_success ||
      !async_completion_error.empty() ||
      async_completion_frame_info.pts_us != frame.pts_us ||
      async_completion_frame_info.target_pixel_buffer_address !=
          static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pixel_buffer)) ||
      async_completion_backend_us > 30'000'000) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend async completion did not report finished BGRA draw");
  }
  if (wgpu_copy_metric_count != 1) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend BGRA draw did not record package copy metrics");
  }
  auto wgpu_stats = wgpu_backend->presentation_stats();
  if (wgpu_stats.present_package_upload_count !=
          wgpu_source_stats_base.present_package_upload_count + 1 ||
      wgpu_stats.last_present_package_storage !=
          VPMacOSNativePresentPackageStorageBGRA ||
      wgpu_stats.last_draw_succeeded == 0 ||
      wgpu_stats.async_metal_publish_active != 1) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend BGRA diagnostics did not update");
  }
  if (wgpu_stats.metal_command_completion_p95_us == 0 ||
      wgpu_stats.metal_command_failure_count !=
          wgpu_source_stats_base.metal_command_failure_count ||
      wgpu_stats.last_present_package_total_us <
          wgpu_stats.last_present_package_copy_us ||
      wgpu_stats.last_present_package_total_us <
          wgpu_stats.last_present_package_gpu_wait_us) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend BGRA command timing diagnostics did not update");
  }
  if (wgpu_stats.video_source_update_count !=
          wgpu_source_stats_base.video_source_update_count ||
      wgpu_stats.source_frame_cache_miss_count !=
          wgpu_source_stats_base.source_frame_cache_miss_count ||
      wgpu_stats.source_frame_cache_hit_count !=
          wgpu_source_stats_base.source_frame_cache_hit_count + 1) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend BGRA source-cache diagnostics did not update");
  }
  std::vector<uint8_t> wgpu_capture;
  int wgpu_capture_width = 0;
  int wgpu_capture_height = 0;
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < 4 || wgpu_capture[0] != 16 ||
      wgpu_capture[1] != 96 || wgpu_capture[2] != 192 ||
      wgpu_capture[3] != 255) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend BGRA capture did not preserve channel order");
  }
  std::vector<uint8_t> wgpu_region_capture;
  int wgpu_region_width = 0;
  int wgpu_region_height = 0;
  if (!wgpu_backend->capture_front_buffer_region(1,
                                                 0,
                                                 1,
                                                 1,
                                                 wgpu_region_capture,
                                                 wgpu_region_width,
                                                 wgpu_region_height) ||
      wgpu_region_width != 1 || wgpu_region_height != 1 ||
      wgpu_region_capture.size() != 4 || wgpu_region_capture[0] != 240 ||
      wgpu_region_capture[1] != 32 || wgpu_region_capture[2] != 64 ||
      wgpu_region_capture[3] != 255) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend BGRA region capture did not preserve channel order");
  }
  if (wgpu_backend->capture_front_buffer_region(kWidth,
                                                kHeight,
                                                1,
                                                1,
                                                wgpu_region_capture,
                                                wgpu_region_width,
                                                wgpu_region_height) ||
      !wgpu_region_capture.empty() || wgpu_region_width != 0 ||
      wgpu_region_height != 0) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend BGRA region capture accepted an empty region");
  }
  auto wgpu_cache_bgra = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(kWidth * kHeight * 4), 0);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    (*wgpu_cache_bgra)[static_cast<size_t>(i) * 4 + 0] = 48;
    (*wgpu_cache_bgra)[static_cast<size_t>(i) * 4 + 1] = 160;
    (*wgpu_cache_bgra)[static_cast<size_t>(i) * 4 + 2] = 224;
    (*wgpu_cache_bgra)[static_cast<size_t>(i) * 4 + 3] = 255;
  }
  vr::TextureFrame wgpu_cache_frame = frame;
  wgpu_cache_frame.pts_us = 124000;
  wgpu_cache_frame.storage = vr::CpuRgbaFrameStorage{wgpu_cache_bgra,
                                                     kWidth * 4};
  vr::RendererDrawSnapshot wgpu_cache_snapshot = snapshot;
  wgpu_cache_snapshot.decision.current_pts_us = wgpu_cache_frame.pts_us;
  wgpu_cache_snapshot.decision.frames[0] = wgpu_cache_frame;
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                wgpu_cache_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected cached BGRA snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_stats = wgpu_backend->presentation_stats();
  if (wgpu_stats.present_package_upload_count !=
          wgpu_source_stats_base.present_package_upload_count + 2 ||
      wgpu_stats.last_draw_succeeded == 0 ||
      wgpu_stats.video_source_update_count !=
          wgpu_source_stats_base.video_source_update_count + 1 ||
      wgpu_stats.source_frame_cache_miss_count !=
          wgpu_source_stats_base.source_frame_cache_miss_count + 1 ||
      wgpu_stats.source_frame_cache_hit_count !=
          wgpu_source_stats_base.source_frame_cache_hit_count + 1) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend cached BGRA diagnostics did not update");
  }
  wgpu_capture.clear();
  wgpu_capture_width = 0;
  wgpu_capture_height = 0;
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < 4 || wgpu_capture[0] != 48 ||
      wgpu_capture[1] != 160 || wgpu_capture[2] != 224 ||
      wgpu_capture[3] != 255) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend cached BGRA draw did not refresh reused resources");
  }
  vr::PresentationBackendDrawHooks wgpu_viewport_hooks;
  wgpu_viewport_hooks.draw_source = "viewport_composite";
  uint64_t wgpu_viewport_copy_metric_count = 0;
  uint64_t wgpu_viewport_copy_metric_us = 1;
  wgpu_viewport_hooks.record_frame_copy_us =
      [&](uint64_t elapsed_us) {
        ++wgpu_viewport_copy_metric_count;
        wgpu_viewport_copy_metric_us = elapsed_us;
      };
  const auto wgpu_before_viewport_stats = wgpu_backend->presentation_stats();
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                wgpu_cache_snapshot,
                                wgpu_viewport_hooks)) {
    std::cerr << "WgpuMetal backend rejected repeated viewport snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_stats = wgpu_backend->presentation_stats();
  if (wgpu_stats.viewport_composite_count !=
          wgpu_source_stats_base.viewport_composite_count + 1 ||
      wgpu_stats.present_package_upload_count !=
          wgpu_before_viewport_stats.present_package_upload_count ||
      wgpu_stats.source_frame_cache_hit_count !=
          wgpu_source_stats_base.source_frame_cache_hit_count + 2 ||
      wgpu_stats.video_source_update_count !=
          wgpu_source_stats_base.video_source_update_count + 1 ||
      wgpu_stats.source_frame_cache_miss_count !=
          wgpu_source_stats_base.source_frame_cache_miss_count + 1) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend source cache diagnostics did not update");
  }
  if (wgpu_viewport_copy_metric_count != 1 || wgpu_viewport_copy_metric_us != 0) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend retained viewport composite did not report zero source copy");
  }
  wgpu_capture.clear();
  wgpu_capture_width = 0;
  wgpu_capture_height = 0;
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < 4 || wgpu_capture[0] != 48 ||
      wgpu_capture[1] != 160 || wgpu_capture[2] != 224 ||
      wgpu_capture[3] != 255) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend retained viewport composite did not reuse cached source");
  }
  auto small_bgra =
      std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(2 * 2 * 4), 0);
  const auto set_small_pixel = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r) {
    const size_t offset = (static_cast<size_t>(y) * 2u + static_cast<size_t>(x)) * 4u;
    (*small_bgra)[offset + 0] = b;
    (*small_bgra)[offset + 1] = g;
    (*small_bgra)[offset + 2] = r;
    (*small_bgra)[offset + 3] = 255;
  };
  set_small_pixel(0, 0, 16, 32, 48);
  set_small_pixel(1, 0, 64, 80, 96);
  set_small_pixel(0, 1, 112, 128, 144);
  set_small_pixel(1, 1, 11, 222, 33);
  vr::TextureFrame small_frame = frame;
  small_frame.width = 2;
  small_frame.height = 2;
  small_frame.storage = vr::CpuRgbaFrameStorage{small_bgra, 2 * 4};
  vr::RendererDrawSnapshot small_source_snapshot = snapshot;
  small_source_snapshot.decision.frames[0] = small_frame;
  small_source_snapshot.tracks[0].video_width = 2;
  small_source_snapshot.tracks[0].video_height = 2;
  small_source_snapshot.track_geometry[0] = {true, 2, 2, 1.0f};
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                small_source_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected source-size BGRA snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_capture.clear();
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < static_cast<size_t>(kWidth * kHeight * 4)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend source-size BGRA capture failed");
  }
  const size_t bottom_right =
      (static_cast<size_t>(kHeight - 1) * kWidth + static_cast<size_t>(kWidth - 1)) * 4u;
  if (wgpu_capture[bottom_right + 0] != 11 ||
      wgpu_capture[bottom_right + 1] != 222 ||
      wgpu_capture[bottom_right + 2] != 33 ||
      wgpu_capture[bottom_right + 3] != 255) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend BGRA source-size sampling read atlas padding");
  }
  auto ring_displayed = make_bgra_pixel_buffer(kWidth, kHeight);
  auto ring_protected = make_bgra_pixel_buffer(kWidth, kHeight);
  auto ring_available = make_bgra_pixel_buffer(kWidth, kHeight);
  if (!ring_displayed.buffer || !ring_protected.buffer ||
      !ring_available.buffer) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend ring smoke could not create target buffers");
  }
  const void* ring_targets[] = {
      ring_displayed.buffer,
      ring_protected.buffer,
      ring_available.buffer,
  };
  if (!wgpu_backend->update_headless_output_ring(ring_targets,
                                                 3,
                                                 ring_displayed.buffer,
                                                 ring_protected.buffer,
                                                 kWidth,
                                                 kHeight,
                                                 3)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not install target ring");
  }
  auto wgpu_ring_diagnostics = wgpu_backend->diagnostics();
  if (wgpu_ring_diagnostics.buffer_count != 3) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend target ring diagnostics did not report buffer count");
  }
  int ring_completion_count = 0;
  uint64_t ring_completion_target = 0;
  vr::PresentationBackendDrawHooks ring_hooks;
  ring_hooks.async_draw_completed =
      [&](bool success,
          const char* error,
          uint64_t,
          const vr::PresentationBackendFrameInfo* frame_info) {
        ++ring_completion_count;
        if (!success || error || !frame_info) {
          ring_completion_target = 0;
          return;
        }
        ring_completion_target = frame_info->target_pixel_buffer_address;
      };
  if (!draw_wgpu_frame_and_wait(*wgpu_backend, snapshot, ring_hooks)) {
    std::cerr << "WgpuMetal backend rejected target ring draw: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  if (ring_completion_count != 1 ||
      ring_completion_target !=
          static_cast<uint64_t>(
              reinterpret_cast<uintptr_t>(ring_available.buffer))) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend target ring draw did not avoid displayed/protected buffers");
  }
  if (wgpu_backend->draw_frame(snapshot, vr::PresentationBackendDrawHooks{})) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend target ring reused completed buffer before release");
  }
  const std::string ring_busy_error = wgpu_backend->last_error();
  if (ring_busy_error.find("wgpu-metal presentation target ring is busy") ==
      std::string::npos) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend target ring busy failure was not diagnostic");
  }
  const auto wgpu_busy_stats = wgpu_backend->presentation_stats();
  if (wgpu_busy_stats.target_installed != 1 ||
      wgpu_busy_stats.metal_buffer_exhaustion_count == 0) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend target ring busy state lost diagnostics");
  }
  wgpu_backend->release_headless_output(ring_available.buffer);
  ring_completion_count = 0;
  ring_completion_target = 0;
  if (!draw_wgpu_frame_and_wait(*wgpu_backend, snapshot, ring_hooks) ||
      ring_completion_count != 1 ||
      ring_completion_target !=
          static_cast<uint64_t>(
              reinterpret_cast<uintptr_t>(ring_available.buffer))) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend target ring did not reuse released completed buffer");
  }
  wgpu_backend->mark_headless_output_displayed(ring_available.buffer);
  ring_completion_count = 0;
  ring_completion_target = 0;
  if (!draw_wgpu_frame_and_wait(*wgpu_backend, snapshot, ring_hooks) ||
      ring_completion_count != 1 ||
      ring_completion_target !=
          static_cast<uint64_t>(
              reinterpret_cast<uintptr_t>(ring_displayed.buffer))) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend target ring did not advance displayed target state");
  }
  wgpu_backend->shutdown();
  if (wgpu_backend->diagnostics().buffer_count != 0 ||
      wgpu_backend->presentation_stats().target_installed != 0) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend target clear diagnostics did not reset");
  }

  constexpr int kLargeTargetWidth = 2196;
  constexpr int kLargeTargetHeight = 1148;
  vr::PresentationBackendConfig large_config = config;
  large_config.width = kLargeTargetWidth;
  large_config.height = kLargeTargetHeight;
  auto large_target =
      make_bgra_pixel_buffer(kLargeTargetWidth, kLargeTargetHeight);
  vr::RendererDrawSnapshot large_snapshot = snapshot;
  large_snapshot.target_width = kLargeTargetWidth;
  large_snapshot.target_height = kLargeTargetHeight;
  if (!large_target.buffer || !wgpu_backend->initialize(large_config) ||
      !wgpu_backend->update_headless_output(large_target.buffer,
                                            kLargeTargetWidth,
                                            kLargeTargetHeight,
                                            3)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend could not initialize >2048 target smoke");
  }
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                large_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected >2048 target snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  if (wgpu_backend->presentation_stats().last_draw_succeeded == 0 ||
      wgpu_backend->diagnostics().width != kLargeTargetWidth ||
      wgpu_backend->diagnostics().height != kLargeTargetHeight) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend >2048 target diagnostics did not update");
  }
  wgpu_backend->shutdown();

  vr::PresentationBackendDrawHooks wgpu_overlay_hooks;
  wgpu_overlay_hooks.build_overlay_primitives =
      [](const vr::RendererDrawSnapshot&) {
        auto package =
            std::make_shared<vr::AnalysisOverlayPrimitivePackage>();
        package->cache_generation = 1;
        package->overlay_track_count = 1;
        package->matched_track_count = 1;
        vr::AnalysisOverlayTrackPrimitives track;
        track.slot = 0;
        track.track_file_id = 7;
        track.video_width = kWidth;
        track.video_height = kHeight;
        track.line_alpha = 255;
        track.fill_rects.push_back(vr::AnalysisOverlayRectPrimitive{
            0,
            0,
            2,
            2,
            vr::analysis::OverlayColor{0, 255, 0, 255},
        });
        package->tracks.push_back(std::move(track));
        return package;
      };
  if (!wgpu_backend->initialize(config)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not reinitialize for overlay draw");
  }
  if (!draw_wgpu_frame_and_wait(*wgpu_backend, snapshot, wgpu_overlay_hooks)) {
    std::cerr << "WgpuMetal backend rejected overlay snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_stats = wgpu_backend->presentation_stats();
  if (wgpu_stats.overlay_last_expected == 0 ||
      wgpu_stats.overlay_last_applied == 0 ||
      wgpu_stats.overlay_last_fill_rect_count != 1 ||
      wgpu_stats.overlay_gpu_success_count != 1) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend overlay diagnostics did not update");
  }
  wgpu_capture.clear();
  wgpu_capture_width = 0;
  wgpu_capture_height = 0;
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < 4 || wgpu_capture[0] > 8 ||
      wgpu_capture[1] < 248 || wgpu_capture[2] > 8 ||
      wgpu_capture[3] != 255) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend WGSL overlay fill did not tint target pixel");
  }
  wgpu_backend->shutdown();

  if (!backend.draw_frame(snapshot, hooks)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend draw_frame did not upload a snapshot");
  }
  if (copy_metric_count != 1 ||
      VPMacOSMetalUploaderPresentPackageUploadCount(backend.uploader()) != 1 ||
      VPMacOSMetalUploaderLastPresentPackageStorage(backend.uploader()) !=
          VPMacOSNativePresentPackageStorageBGRA) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend draw_frame diagnostics did not update");
  }

  auto second_bgra = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(kWidth * kHeight * 4), 0);
  for (int i = 0; i < kWidth * kHeight; ++i) {
    (*second_bgra)[static_cast<size_t>(i) * 4 + 0] = 192;
    (*second_bgra)[static_cast<size_t>(i) * 4 + 1] = 96;
    (*second_bgra)[static_cast<size_t>(i) * 4 + 2] = 16;
    (*second_bgra)[static_cast<size_t>(i) * 4 + 3] = 255;
  }
  vr::TextureFrame second_frame = frame;
  second_frame.pts_us = frame.pts_us;
  second_frame.storage = vr::CpuRgbaFrameStorage{second_bgra, kWidth * 4};

  vr::RendererDrawSnapshot partial_snapshot = snapshot;
  partial_snapshot.decision.frames[1] = second_frame;
  partial_snapshot.decision.file_ids[1] = 8;
  partial_snapshot.decision.track_generations[1] = 1;
  partial_snapshot.tracks[1].active = true;
  partial_snapshot.tracks[1].file_id = 8;
  partial_snapshot.tracks[1].generation = 1;
  partial_snapshot.tracks[1].video_width = kWidth;
  partial_snapshot.tracks[1].video_height = kHeight;
  partial_snapshot.tracks[1].video_aspect = 1.0f;
  partial_snapshot.tracks[2].active = true;
  partial_snapshot.tracks[2].file_id = 9;
  partial_snapshot.tracks[2].generation = 1;
  partial_snapshot.tracks[2].video_width = kWidth;
  partial_snapshot.tracks[2].video_height = kHeight;
  partial_snapshot.tracks[2].video_aspect = 1.0f;
  partial_snapshot.track_geometry[1] = {true, kWidth, kHeight, 1.0f};
  partial_snapshot.track_geometry[2] = {true, kWidth, kHeight, 1.0f};
  vr::RendererDrawSnapshot wgpu_split_snapshot = partial_snapshot;
  wgpu_split_snapshot.layout.mode = vr::LAYOUT_SPLIT_SCREEN;
  wgpu_split_snapshot.layout.split_pos = 0.5f;
  wgpu_split_snapshot.layout.order[0] = 0;
  wgpu_split_snapshot.layout.order[1] = 1;
  if (!wgpu_backend->initialize(config)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not reinitialize for split BGRA draw");
  }
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                wgpu_split_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected split BGRA snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_capture.clear();
  wgpu_capture_width = 0;
  wgpu_capture_height = 0;
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < static_cast<size_t>(kWidth * kHeight * 4)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend split BGRA capture failed");
  }
  const auto pixel_at = [&](int x, int y, int channel) -> uint8_t {
    return wgpu_capture[(static_cast<size_t>(y) * kWidth + x) * 4 +
                        static_cast<size_t>(channel)];
  };
  if (pixel_at(0, 0, 0) <= pixel_at(kWidth - 1, 0, 0) ||
      pixel_at(0, 0, 2) >= pixel_at(kWidth - 1, 0, 2)) {
    std::cerr << "WgpuMetal split pixels left=("
              << static_cast<int>(pixel_at(0, 0, 0)) << ","
              << static_cast<int>(pixel_at(0, 0, 1)) << ","
              << static_cast<int>(pixel_at(0, 0, 2)) << ") right=("
              << static_cast<int>(pixel_at(kWidth - 1, 0, 0)) << ","
              << static_cast<int>(pixel_at(kWidth - 1, 0, 1)) << ","
              << static_cast<int>(pixel_at(kWidth - 1, 0, 2)) << ")\n";
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend split BGRA WGSL layout sampled wrong tracks");
  }
  wgpu_backend->shutdown();
  if (!backend.draw_frame(partial_snapshot, hooks)) {
    std::cerr << "Metal presentation backend rejected partial multi-track "
                 "snapshot: "
              << backend.last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  if (copy_metric_count != 2 ||
      VPMacOSMetalUploaderPresentPackageUploadCount(backend.uploader()) != 2 ||
      VPMacOSMetalUploaderLastPresentPackageStorage(backend.uploader()) !=
          VPMacOSNativePresentPackageStorageBGRA) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend partial multi-track diagnostics did not update");
  }

  auto nv12 = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(kWidth * kHeight + kWidth * (kHeight / 2)), 0);
  const uint8_t nv12_y[] = {
      82, 145, 41, 210,
      96, 160, 58, 224,
      40, 118, 74, 188,
      64, 132, 92, 206,
  };
  std::copy(std::begin(nv12_y), std::end(nv12_y), nv12->begin());
  const size_t nv12_uv_offset = static_cast<size_t>(kWidth * kHeight);
  // Four 2x2 chroma blocks: U,V interleaved.
  (*nv12)[nv12_uv_offset + 0] = 90;
  (*nv12)[nv12_uv_offset + 1] = 240;
  (*nv12)[nv12_uv_offset + 2] = 200;
  (*nv12)[nv12_uv_offset + 3] = 60;
  (*nv12)[nv12_uv_offset + 4] = 54;
  (*nv12)[nv12_uv_offset + 5] = 150;
  (*nv12)[nv12_uv_offset + 6] = 180;
  (*nv12)[nv12_uv_offset + 7] = 200;
  vr::TextureFrame nv12_frame;
  nv12_frame.width = kWidth;
  nv12_frame.height = kHeight;
  nv12_frame.pts_us = 156333;
  nv12_frame.duration_us = 33333;
  nv12_frame.is_nv12 = true;
  nv12_frame.color = {vr::VIDEO_COLOR_RANGE_LIMITED,
                      vr::VIDEO_COLOR_MATRIX_BT601,
                      vr::VIDEO_COLOR_TRANSFER_SDR,
                      vr::VIDEO_COLOR_PRIMARIES_BT601};
  nv12_frame.storage = vr::CpuNv12FrameStorage{
      nv12,
      kWidth,
      kWidth,
      false,
      kWidth,
      kHeight,
  };
  vr::RendererDrawSnapshot nv12_snapshot = snapshot;
  nv12_snapshot.decision.current_pts_us = nv12_frame.pts_us;
  nv12_snapshot.decision.frames[0] = nv12_frame;
  if (!backend.draw_frame(nv12_snapshot, hooks)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend draw_frame did not upload NV12 snapshot");
  }
  if (copy_metric_count != 3 ||
      VPMacOSMetalUploaderPresentPackageUploadCount(backend.uploader()) != 3 ||
      VPMacOSMetalUploaderLastPresentPackageStorage(backend.uploader()) !=
          VPMacOSNativePresentPackageStorageYUV) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend NV12 draw_frame diagnostics did not update");
  }
  if (!wgpu_backend->initialize(config)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not reinitialize for NV12 draw");
  }
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                nv12_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected NV12 snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_stats = wgpu_backend->presentation_stats();
  if (wgpu_stats.last_present_package_storage !=
          VPMacOSNativePresentPackageStorageYUV ||
      wgpu_stats.last_draw_succeeded == 0) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend NV12 diagnostics did not update");
  }
  wgpu_capture.clear();
  wgpu_capture_width = 0;
  wgpu_capture_height = 0;
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < static_cast<size_t>(kWidth * kHeight * 4) ||
      !expect_bgra_near(
          "WgpuMetal NV12 limited BT601",
          wgpu_capture,
          wgpu_capture_width,
          0,
          0,
          reference_yuv_to_bgra(nv12_y[0],
                                (*nv12)[nv12_uv_offset + 0],
                                (*nv12)[nv12_uv_offset + 1],
                                vr::VIDEO_COLOR_RANGE_LIMITED,
                                vr::VIDEO_COLOR_MATRIX_BT601),
          4) ||
      !expect_bgra_near(
          "WgpuMetal NV12 limited BT601",
          wgpu_capture,
          wgpu_capture_width,
          2,
          0,
          reference_yuv_to_bgra(nv12_y[2],
                                (*nv12)[nv12_uv_offset + 2],
                                (*nv12)[nv12_uv_offset + 3],
                                vr::VIDEO_COLOR_RANGE_LIMITED,
                                vr::VIDEO_COLOR_MATRIX_BT601),
          4) ||
      !expect_bgra_near(
          "WgpuMetal NV12 limited BT601",
          wgpu_capture,
          wgpu_capture_width,
          3,
          3,
          reference_yuv_to_bgra(nv12_y[15],
                                (*nv12)[nv12_uv_offset + 6],
                                (*nv12)[nv12_uv_offset + 7],
                                vr::VIDEO_COLOR_RANGE_LIMITED,
                                vr::VIDEO_COLOR_MATRIX_BT601),
          4)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend NV12 limited BT601 color parity failed");
  }

  auto nv12_full = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(kWidth * kHeight + kWidth * (kHeight / 2)), 0);
  const uint8_t nv12_full_y[] = {
      30, 100, 180, 250,
      45, 115, 195, 235,
      64, 96, 128, 160,
      80, 112, 144, 176,
  };
  std::copy(std::begin(nv12_full_y),
            std::end(nv12_full_y),
            nv12_full->begin());
  (*nv12_full)[nv12_uv_offset + 0] = 80;
  (*nv12_full)[nv12_uv_offset + 1] = 220;
  (*nv12_full)[nv12_uv_offset + 2] = 220;
  (*nv12_full)[nv12_uv_offset + 3] = 80;
  (*nv12_full)[nv12_uv_offset + 4] = 100;
  (*nv12_full)[nv12_uv_offset + 5] = 140;
  (*nv12_full)[nv12_uv_offset + 6] = 160;
  (*nv12_full)[nv12_uv_offset + 7] = 200;
  vr::TextureFrame nv12_full_frame = nv12_frame;
  nv12_full_frame.pts_us = 172000;
  nv12_full_frame.color = {vr::VIDEO_COLOR_RANGE_FULL,
                           vr::VIDEO_COLOR_MATRIX_BT709,
                           vr::VIDEO_COLOR_TRANSFER_SDR,
                           vr::VIDEO_COLOR_PRIMARIES_BT709};
  nv12_full_frame.storage = vr::CpuNv12FrameStorage{
      nv12_full,
      kWidth,
      kWidth,
      false,
      kWidth,
      kHeight,
  };
  vr::RendererDrawSnapshot nv12_full_snapshot = snapshot;
  nv12_full_snapshot.decision.current_pts_us = nv12_full_frame.pts_us;
  nv12_full_snapshot.decision.frames[0] = nv12_full_frame;
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                nv12_full_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected full-range NV12 snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_capture.clear();
  wgpu_capture_width = 0;
  wgpu_capture_height = 0;
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < static_cast<size_t>(kWidth * kHeight * 4) ||
      !expect_bgra_near(
          "WgpuMetal NV12 full BT709",
          wgpu_capture,
          wgpu_capture_width,
          0,
          0,
          reference_yuv_to_bgra(nv12_full_y[0],
                                (*nv12_full)[nv12_uv_offset + 0],
                                (*nv12_full)[nv12_uv_offset + 1],
                                vr::VIDEO_COLOR_RANGE_FULL,
                                vr::VIDEO_COLOR_MATRIX_BT709),
          4) ||
      !expect_bgra_near(
          "WgpuMetal NV12 full BT709",
          wgpu_capture,
          wgpu_capture_width,
          2,
          0,
          reference_yuv_to_bgra(nv12_full_y[2],
                                (*nv12_full)[nv12_uv_offset + 2],
                                (*nv12_full)[nv12_uv_offset + 3],
                                vr::VIDEO_COLOR_RANGE_FULL,
                                vr::VIDEO_COLOR_MATRIX_BT709),
          4) ||
      !expect_bgra_near(
          "WgpuMetal NV12 full BT709",
          wgpu_capture,
          wgpu_capture_width,
          3,
          3,
          reference_yuv_to_bgra(nv12_full_y[15],
                                (*nv12_full)[nv12_uv_offset + 6],
                                (*nv12_full)[nv12_uv_offset + 7],
                                vr::VIDEO_COLOR_RANGE_FULL,
                                vr::VIDEO_COLOR_MATRIX_BT709),
          4)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend NV12 full BT709 color parity failed");
  }
  wgpu_backend->shutdown();

  auto p010 = std::make_shared<std::vector<uint8_t>>(
      static_cast<size_t>(kWidth * kHeight * 2 + kWidth * (kHeight / 2) * 2),
      0);
  const int p010_y_stride = kWidth * 2;
  const int p010_uv_stride = kWidth * 2;
  const uint16_t p010_y_codes[] = {
      328, 512, 720, 900,
      360, 544, 752, 872,
      280, 420, 620, 820,
      300, 460, 660, 860,
  };
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      write_p010_sample(*p010,
                        static_cast<size_t>(y) * p010_y_stride +
                            static_cast<size_t>(x) * 2u,
                        p010_y_codes[y * kWidth + x]);
    }
  }
  const size_t p010_uv_offset =
      static_cast<size_t>(p010_y_stride) * static_cast<size_t>(kHeight);
  const uint16_t p010_uv_codes[] = {
      360, 880,
      800, 320,
      420, 580,
      680, 760,
  };
  for (int y = 0; y < kHeight / 2; ++y) {
    for (int x = 0; x < kWidth / 2; ++x) {
      const size_t offset = p010_uv_offset +
          static_cast<size_t>(y) * p010_uv_stride +
          static_cast<size_t>(x) * 4u;
      const int block = y * (kWidth / 2) + x;
      write_p010_sample(*p010, offset, p010_uv_codes[block * 2]);
      write_p010_sample(*p010, offset + 2, p010_uv_codes[block * 2 + 1]);
    }
  }
  vr::TextureFrame p010_frame = nv12_frame;
  p010_frame.pts_us = 189666;
  p010_frame.is_p010 = true;
  p010_frame.color = {vr::VIDEO_COLOR_RANGE_LIMITED,
                      vr::VIDEO_COLOR_MATRIX_BT709,
                      vr::VIDEO_COLOR_TRANSFER_SDR,
                      vr::VIDEO_COLOR_PRIMARIES_BT709};
  p010_frame.storage = vr::CpuNv12FrameStorage{
      p010,
      p010_y_stride,
      p010_uv_stride,
      true,
      kWidth,
      kHeight,
  };
  vr::RendererDrawSnapshot p010_snapshot = snapshot;
  p010_snapshot.decision.current_pts_us = p010_frame.pts_us;
  p010_snapshot.decision.frames[0] = p010_frame;
  if (!wgpu_backend->initialize(config)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not reinitialize for P010 draw");
  }
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                p010_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected P010 snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_capture.clear();
  wgpu_capture_width = 0;
  wgpu_capture_height = 0;
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < static_cast<size_t>(kWidth * kHeight * 4) ||
      !expect_bgra_near(
          "WgpuMetal P010 limited BT709",
          wgpu_capture,
          wgpu_capture_width,
          0,
          0,
          reference_yuv_to_bgra(
              static_cast<uint8_t>(p010_y_codes[0] >> 2),
              static_cast<uint8_t>(p010_uv_codes[0] >> 2),
              static_cast<uint8_t>(p010_uv_codes[1] >> 2),
              vr::VIDEO_COLOR_RANGE_LIMITED,
              vr::VIDEO_COLOR_MATRIX_BT709),
          5) ||
      !expect_bgra_near(
          "WgpuMetal P010 limited BT709",
          wgpu_capture,
          wgpu_capture_width,
          2,
          0,
          reference_yuv_to_bgra(
              static_cast<uint8_t>(p010_y_codes[2] >> 2),
              static_cast<uint8_t>(p010_uv_codes[2] >> 2),
              static_cast<uint8_t>(p010_uv_codes[3] >> 2),
              vr::VIDEO_COLOR_RANGE_LIMITED,
              vr::VIDEO_COLOR_MATRIX_BT709),
          5) ||
      !expect_bgra_near(
          "WgpuMetal P010 limited BT709",
          wgpu_capture,
          wgpu_capture_width,
          3,
          3,
          reference_yuv_to_bgra(
              static_cast<uint8_t>(p010_y_codes[15] >> 2),
              static_cast<uint8_t>(p010_uv_codes[6] >> 2),
              static_cast<uint8_t>(p010_uv_codes[7] >> 2),
              vr::VIDEO_COLOR_RANGE_LIMITED,
              vr::VIDEO_COLOR_MATRIX_BT709),
          5)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend P010 limited BT709 color parity failed");
  }
  vr::TextureFrame p010_hdr_frame = p010_frame;
  p010_hdr_frame.pts_us = 190000;
  p010_hdr_frame.color = {vr::VIDEO_COLOR_RANGE_LIMITED,
                          vr::VIDEO_COLOR_MATRIX_BT2020_NCL,
                          vr::VIDEO_COLOR_TRANSFER_PQ,
                          vr::VIDEO_COLOR_PRIMARIES_BT2020};
  vr::RendererDrawSnapshot p010_hdr_snapshot = p010_snapshot;
  p010_hdr_snapshot.decision.current_pts_us = p010_hdr_frame.pts_us;
  p010_hdr_snapshot.decision.frames[0] = p010_hdr_frame;
  const auto p010_failure_count_before =
      wgpu_backend->presentation_stats().draw_failure_count;
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                p010_hdr_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected P010 HDR snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  if (wgpu_backend->presentation_stats().last_draw_succeeded == 0 ||
      wgpu_backend->presentation_stats().draw_failure_count !=
          p010_failure_count_before) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend P010 HDR SDR tone-map draw did not report success");
  }
  auto p010_edr_target = make_rgba_half_pixel_buffer(kWidth, kHeight);
  if (!p010_edr_target.buffer ||
      !wgpu_backend->update_headless_output(p010_edr_target.buffer,
                                            kWidth,
                                            kHeight,
                                            3)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend could not install P010 HDR EDR target");
  }
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                p010_hdr_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected P010 HDR EDR snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  const auto p010_edr_diagnostics = wgpu_backend->diagnostics();
  if (wgpu_backend->presentation_stats().last_draw_succeeded == 0 ||
      std::string(p010_edr_diagnostics.render_target_format)
              .find("RGBA16Float") == std::string::npos ||
      std::string(p010_edr_diagnostics.render_color_space).find("edr") ==
          std::string::npos) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend P010 HDR EDR draw did not report success");
  }
  float p010_edr_max_rgb = 0.0f;
  int p010_edr_pixels_over_one = 0;
  int p010_edr_sample_count = 0;
  if (!measure_rgba_half_edr(p010_edr_target.buffer,
                             &p010_edr_max_rgb,
                             &p010_edr_pixels_over_one,
                             &p010_edr_sample_count) ||
      p010_edr_sample_count != kWidth * kHeight ||
      p010_edr_max_rgb <= 1.0f ||
      p010_edr_pixels_over_one <= 0) {
    std::cerr << "WgpuMetal backend P010 HDR EDR target did not expose "
              << "extended-range samples: max_rgb=" << p010_edr_max_rgb
              << " pixels_over_one=" << p010_edr_pixels_over_one
              << " sample_count=" << p010_edr_sample_count << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  vr::TextureFrame p010_hlg_frame = p010_frame;
  p010_hlg_frame.pts_us = 191000;
  p010_hlg_frame.color = {vr::VIDEO_COLOR_RANGE_LIMITED,
                          vr::VIDEO_COLOR_MATRIX_BT2020_NCL,
                          vr::VIDEO_COLOR_TRANSFER_HLG,
                          vr::VIDEO_COLOR_PRIMARIES_BT2020};
  vr::RendererDrawSnapshot p010_hlg_snapshot = p010_snapshot;
  p010_hlg_snapshot.decision.current_pts_us = p010_hlg_frame.pts_us;
  p010_hlg_snapshot.decision.frames[0] = p010_hlg_frame;
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                p010_hlg_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected P010 HLG EDR snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  const auto p010_hlg_edr_diagnostics = wgpu_backend->diagnostics();
  if (wgpu_backend->presentation_stats().last_draw_succeeded == 0 ||
      std::string(p010_hlg_edr_diagnostics.render_target_format)
              .find("RGBA16Float") == std::string::npos ||
      std::string(p010_hlg_edr_diagnostics.render_color_space).find("edr") ==
          std::string::npos) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend P010 HLG EDR draw did not report success");
  }
  float p010_hlg_edr_max_rgb = 0.0f;
  int p010_hlg_edr_pixels_over_one = 0;
  int p010_hlg_edr_sample_count = 0;
  if (!measure_rgba_half_edr(p010_edr_target.buffer,
                             &p010_hlg_edr_max_rgb,
                             &p010_hlg_edr_pixels_over_one,
                             &p010_hlg_edr_sample_count) ||
      p010_hlg_edr_sample_count != kWidth * kHeight ||
      p010_hlg_edr_max_rgb <= 1.0f ||
      p010_hlg_edr_pixels_over_one <= 0) {
    std::cerr << "WgpuMetal backend P010 HLG EDR target did not expose "
              << "extended-range samples: max_rgb=" << p010_hlg_edr_max_rgb
              << " pixels_over_one=" << p010_hlg_edr_pixels_over_one
              << " sample_count=" << p010_hlg_edr_sample_count << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_backend->shutdown();

  auto planar_y = std::make_shared<std::array<uint8_t, 4>>(
      std::array<uint8_t, 4>{76, 76, 76, 76});
  auto planar_u =
      std::make_shared<std::array<uint8_t, 1>>(std::array<uint8_t, 1>{85});
  auto planar_v =
      std::make_shared<std::array<uint8_t, 1>>(std::array<uint8_t, 1>{255});
  vr::TextureFrame planar_frame;
  planar_frame.width = 2;
  planar_frame.height = 2;
  planar_frame.pts_us = 223000;
  planar_frame.duration_us = 33333;
  planar_frame.color.range = vr::VIDEO_COLOR_RANGE_FULL;
  planar_frame.color.matrix = vr::VIDEO_COLOR_MATRIX_BT601;
  planar_frame.storage = vr::CpuPlanarYuvFrameStorage{
      planar_y,
      {planar_y->data(), planar_u->data(), planar_v->data()},
      {2, 1, 1},
      {2, 1, 1},
      {2, 1, 1},
      1,
  };
  vr::RendererDrawSnapshot planar_snapshot = snapshot;
  planar_snapshot.decision.current_pts_us = planar_frame.pts_us;
  planar_snapshot.decision.frames[0] = planar_frame;
  planar_snapshot.tracks[0].video_width = planar_frame.width;
  planar_snapshot.tracks[0].video_height = planar_frame.height;
  planar_snapshot.tracks[0].video_aspect = 1.0f;
  planar_snapshot.track_geometry[0] = {true, planar_frame.width,
                                       planar_frame.height, 1.0f};
  if (!wgpu_backend->initialize(config)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not reinitialize for planar YUV draw");
  }
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                planar_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected planar YUV snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_capture.clear();
  wgpu_capture_width = 0;
  wgpu_capture_height = 0;
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < 4 || wgpu_capture[0] > 4 ||
      wgpu_capture[1] > 4 || wgpu_capture[2] < 248 ||
      wgpu_capture[3] != 255) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend planar YUV WGSL capture did not produce red BGRA output");
  }
  wgpu_backend->shutdown();
  const auto backend_stats = backend.presentation_stats();
  if (backend_stats.staging_allocation_count == 0 ||
      backend_stats.staging_reuse_count == 0 ||
      backend_stats.staging_max_bytes == 0) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend staging diagnostics did not update");
  }

  auto left_cv = make_nv12_pixel_buffer(kWidth, kHeight, 96, 128, 128);
  auto right_cv = make_nv12_pixel_buffer(kWidth, kHeight, 180, 128, 128);
  if (!left_cv.buffer || !right_cv.buffer) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend could not create CVPixelBuffer sources");
  }
  vr::RendererDrawSnapshot cv_snapshot = snapshot;
  const auto left_cv_frame =
      make_cv_nv12_frame(left_cv.buffer, kWidth, kHeight, 200000);
  const auto right_cv_frame =
      make_cv_nv12_frame(right_cv.buffer, kWidth, kHeight, 200000);
  cv_snapshot.layout.mode = vr::LAYOUT_SPLIT_SCREEN;
  cv_snapshot.layout.pixel_size_mode = vr::PIXEL_SIZE_FILL_VIEW;
  cv_snapshot.layout.order[0] = 0;
  cv_snapshot.layout.order[1] = 1;
  cv_snapshot.decision.current_pts_us = left_cv_frame.pts_us;
  cv_snapshot.decision.frames[0] = left_cv_frame;
  cv_snapshot.decision.frames[1] = right_cv_frame;
  cv_snapshot.decision.file_ids[1] = 8;
  cv_snapshot.decision.track_generations[1] = 1;
  cv_snapshot.tracks[1].active = true;
  cv_snapshot.tracks[1].file_id = 8;
  cv_snapshot.tracks[1].generation = 1;
  cv_snapshot.tracks[1].video_width = kWidth;
  cv_snapshot.tracks[1].video_height = kHeight;
  cv_snapshot.tracks[1].video_aspect = 1.0f;
  cv_snapshot.track_geometry[1] = {true, kWidth, kHeight, 1.0f};
  const int64_t cv_upload_count_before =
      VPMacOSMetalUploaderCVPixelBufferUploadCount(backend.uploader());
  if (!backend.draw_frame(cv_snapshot, hooks)) {
    std::cerr << "Metal presentation backend rejected multi-track "
                 "CVPixelBuffer snapshot: "
              << backend.last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  if (copy_metric_count != 4 ||
      VPMacOSMetalUploaderPresentPackageUploadCount(backend.uploader()) != 3 ||
      VPMacOSMetalUploaderCVPixelBufferUploadCount(backend.uploader()) !=
          cv_upload_count_before + 1 ||
      VPMacOSMetalUploaderLastPresentPackageStorage(backend.uploader()) !=
          VPMacOSNativePresentPackageStorageCVPixelBuffer ||
      VPMacOSMetalUploaderLastPresentPackageCopyUs(backend.uploader()) != 0) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend multi-track CVPixelBuffer diagnostics did not update");
  }
  if (!wgpu_backend->initialize(config)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not reinitialize for CVPixelBuffer draw");
  }
  uint64_t wgpu_cv_copy_metric_count = 0;
  uint64_t wgpu_cv_copy_metric_us = 1;
  vr::PresentationBackendDrawHooks wgpu_cv_hooks;
  wgpu_cv_hooks.record_frame_copy_us =
      [&](uint64_t elapsed_us) {
        ++wgpu_cv_copy_metric_count;
        wgpu_cv_copy_metric_us = elapsed_us;
      };
  if (!draw_wgpu_frame_and_wait(*wgpu_backend, cv_snapshot, wgpu_cv_hooks)) {
    std::cerr << "WgpuMetal backend rejected CVPixelBuffer snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_stats = wgpu_backend->presentation_stats();
  if (wgpu_stats.last_present_package_storage !=
          VPMacOSNativePresentPackageStorageCVPixelBuffer ||
      wgpu_stats.last_draw_succeeded == 0 ||
      wgpu_stats.last_present_package_copy_us != 0 ||
      wgpu_stats.last_present_package_gpu_wait_us <= 0 ||
      wgpu_stats.last_present_package_total_us <
          wgpu_stats.last_present_package_gpu_wait_us) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend CVPixelBuffer diagnostics did not update");
  }
  if (wgpu_cv_copy_metric_count != 1 || wgpu_cv_copy_metric_us != 0) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend CVPixelBuffer draw did not record zero-copy metrics");
  }
  wgpu_capture.clear();
  wgpu_capture_width = 0;
  wgpu_capture_height = 0;
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < static_cast<size_t>(kWidth * kHeight * 4)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend CVPixelBuffer capture failed");
  }
  const auto cv_pixel_at = [&](int x, int y, int channel) -> uint8_t {
    return wgpu_capture[(static_cast<size_t>(y) * kWidth + x) * 4 +
                        static_cast<size_t>(channel)];
  };
  const int left_luma = static_cast<int>(cv_pixel_at(0, 0, 1));
  const int right_luma = static_cast<int>(cv_pixel_at(kWidth - 1, 0, 1));
  if (std::abs(left_luma - right_luma) < 16 ||
      (left_luma < 8 && right_luma < 8)) {
    std::cerr << "WgpuMetal CVPixelBuffer split pixels left=("
              << static_cast<int>(cv_pixel_at(0, 0, 0)) << ","
              << static_cast<int>(cv_pixel_at(0, 0, 1)) << ","
              << static_cast<int>(cv_pixel_at(0, 0, 2)) << ") right=("
              << static_cast<int>(cv_pixel_at(kWidth - 1, 0, 0)) << ","
              << static_cast<int>(cv_pixel_at(kWidth - 1, 0, 1)) << ","
              << static_cast<int>(cv_pixel_at(kWidth - 1, 0, 2)) << ")\n";
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend CVPixelBuffer WGSL layout did not sample distinct tracks");
  }
  wgpu_backend->shutdown();

  const uint16_t cv_p010_y_codes[] = {
      320, 500, 700, 880,
      340, 520, 720, 860,
      280, 430, 640, 810,
      300, 460, 660, 840,
  };
  const uint16_t cv_p010_uv_codes[] = {
      360, 860,
      780, 340,
      440, 600,
      700, 760,
  };
  auto p010_cv = make_p010_pixel_buffer(kWidth,
                                        kHeight,
                                        cv_p010_y_codes,
                                        cv_p010_uv_codes);
  if (!p010_cv.buffer) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend could not create P010 CVPixelBuffer source");
  }
  vr::RendererDrawSnapshot cv_p010_snapshot = snapshot;
  const auto cv_p010_frame =
      make_cv_p010_frame(p010_cv.buffer, kWidth, kHeight, 240000);
  cv_p010_snapshot.decision.current_pts_us = cv_p010_frame.pts_us;
  cv_p010_snapshot.decision.frames[0] = cv_p010_frame;
  if (!backend.draw_frame(cv_p010_snapshot, hooks)) {
    std::cerr << "Metal presentation backend rejected P010 CVPixelBuffer "
                 "snapshot: "
              << backend.last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  if (VPMacOSMetalUploaderLastPresentPackageStorage(backend.uploader()) !=
      VPMacOSNativePresentPackageStorageCVPixelBuffer) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend P010 CVPixelBuffer diagnostics did not update");
  }
  if (!wgpu_backend->initialize(config)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend did not reinitialize for P010 CVPixelBuffer draw");
  }
  if (!draw_wgpu_frame_and_wait(*wgpu_backend,
                                cv_p010_snapshot,
                                vr::PresentationBackendDrawHooks{})) {
    std::cerr << "WgpuMetal backend rejected P010 CVPixelBuffer snapshot: "
              << wgpu_backend->last_error() << "\n";
    CVPixelBufferRelease(pixel_buffer);
    return 1;
  }
  wgpu_stats = wgpu_backend->presentation_stats();
  if (wgpu_stats.last_present_package_storage !=
          VPMacOSNativePresentPackageStorageCVPixelBuffer ||
      wgpu_stats.cvpixelbuffer_upload_count == 0 ||
      wgpu_stats.last_draw_succeeded == 0) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend P010 CVPixelBuffer diagnostics did not update");
  }
  wgpu_capture.clear();
  wgpu_capture_width = 0;
  wgpu_capture_height = 0;
  if (!wgpu_backend->capture_front_buffer(wgpu_capture,
                                          wgpu_capture_width,
                                          wgpu_capture_height) ||
      wgpu_capture_width != kWidth || wgpu_capture_height != kHeight ||
      wgpu_capture.size() < static_cast<size_t>(kWidth * kHeight * 4) ||
      !expect_bgra_near(
          "WgpuMetal P010 CVPixelBuffer limited BT709",
          wgpu_capture,
          wgpu_capture_width,
          0,
          0,
          reference_yuv_to_bgra(
              static_cast<uint8_t>(cv_p010_y_codes[0] >> 2),
              static_cast<uint8_t>(cv_p010_uv_codes[0] >> 2),
              static_cast<uint8_t>(cv_p010_uv_codes[1] >> 2),
              vr::VIDEO_COLOR_RANGE_LIMITED,
              vr::VIDEO_COLOR_MATRIX_BT709),
          6) ||
      !expect_bgra_near(
          "WgpuMetal P010 CVPixelBuffer limited BT709",
          wgpu_capture,
          wgpu_capture_width,
          3,
          3,
          reference_yuv_to_bgra(
              static_cast<uint8_t>(cv_p010_y_codes[15] >> 2),
              static_cast<uint8_t>(cv_p010_uv_codes[6] >> 2),
              static_cast<uint8_t>(cv_p010_uv_codes[7] >> 2),
              vr::VIDEO_COLOR_RANGE_LIMITED,
              vr::VIDEO_COLOR_MATRIX_BT709),
          6)) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("WgpuMetal backend P010 CVPixelBuffer color parity failed");
  }
  wgpu_backend->shutdown();

  backend.shutdown();
  if (backend.available() || backend.uploader()) {
    CVPixelBufferRelease(pixel_buffer);
    return fail("Metal presentation backend did not release uploader");
  }
  CVPixelBufferRelease(pixel_buffer);

  VPMacOSMetalPresentationBackend* c_backend =
      VPMacOSMetalPresentationBackendCreate(640, 360);
  if (!c_backend) {
    return fail("Metal presentation backend C ABI did not initialize");
  }
  if (VPMacOSMetalPresentationBackendIsAvailable(c_backend) == 0 ||
      !VPMacOSMetalPresentationBackendUploader(c_backend)) {
    VPMacOSMetalPresentationBackendDestroy(c_backend);
    return fail("Metal presentation backend C ABI uploader is unavailable");
  }
  if (VPMacOSMetalPresentationBackendDirectYUVUploadCount(c_backend) != 0) {
    VPMacOSMetalPresentationBackendDestroy(c_backend);
    return fail("Metal presentation backend initial YUV upload count is not zero");
  }
  if (VPMacOSMetalPresentationBackendPresentPackageUploadCount(c_backend) != 0 ||
      VPMacOSMetalPresentationBackendLastPresentPackageStorage(c_backend) !=
          VPMacOSNativePresentPackageStorageUnavailable) {
    VPMacOSMetalPresentationBackendDestroy(c_backend);
    return fail("Metal presentation backend initial package diagnostics are not empty");
  }
  VPMacOSMetalPresentationBackendDestroy(c_backend);
  return 0;
}
