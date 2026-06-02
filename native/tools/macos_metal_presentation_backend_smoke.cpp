#include "macos/metal/metal_presentation_backend.h"

#include "renderer/render/presentation_backend_factory.h"
#include "renderer/render/renderer_draw_snapshot.h"

#include <CoreVideo/CoreVideo.h>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int fail(const char* message) {
  std::cerr << message << "\n";
  return 1;
}

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
  vr::PresentationBackendConfig config;
  config.output = pixel_buffer;
  config.width = kWidth;
  config.height = kHeight;
  config.max_track_slots = 3;
  config.headless = true;

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
      static_cast<size_t>(kWidth * kHeight + kWidth * (kHeight / 2)), 128);
  vr::TextureFrame nv12_frame;
  nv12_frame.width = kWidth;
  nv12_frame.height = kHeight;
  nv12_frame.pts_us = 156333;
  nv12_frame.duration_us = 33333;
  nv12_frame.is_nv12 = true;
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
  cv_snapshot.layout.order[0] = 7;
  cv_snapshot.layout.order[1] = 8;
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
