#include "macos/metal_presentation_backend.h"

#include "video_renderer/render/renderer_draw_snapshot.h"

#include <CoreVideo/CoreVideo.h>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

int fail(const char* message) {
  std::cerr << message << "\n";
  return 1;
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
  vr::PresentationBackendConfig config;
  config.output = pixel_buffer;
  config.width = kWidth;
  config.height = kHeight;
  config.max_track_slots = 1;
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
