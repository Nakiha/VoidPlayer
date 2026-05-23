#include "macos/native_player_bridge.h"
#include "tools/test_video_assets.h"

#include <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>

#ifndef VIDEO_TEST_DIR
#define VIDEO_TEST_DIR ""
#endif

namespace {

int fail(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

bool expect_validation(VPMacOSMetalUploader* uploader,
                       CVPixelBufferRef buffer,
                       int width,
                       int height,
                       int expected_status,
                       const char* expected_message) {
  char error[256] = {};
  const int status = VPMacOSMetalUploaderValidatePixelBufferChecked(
      uploader, buffer, width, height, error, sizeof(error));
  return status == expected_status &&
      std::strcmp(error, expected_message) == 0 &&
      std::strcmp(VPMacOSMetalUploaderStatusMessage(status), expected_message) == 0;
}

CVPixelBufferRef create_pixel_buffer(OSType pixel_format, int width, int height) {
  NSDictionary* attributes = @{
    (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
    (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
  };
  CVPixelBufferRef buffer = nullptr;
  const CVReturn status = CVPixelBufferCreate(
      kCFAllocatorDefault,
      width,
      height,
      pixel_format,
      (__bridge CFDictionaryRef)attributes,
      &buffer);
  if (status != kCVReturnSuccess) {
    return nullptr;
  }
  return buffer;
}

bool copy_frame_with_layout(VPMacOSMetalUploader* uploader,
                            VPMacOSNativePlayer* player,
                            CVPixelBufferRef buffer,
                            int width,
                            int height,
                            VPMacOSNativeFrameInfo* out,
                            char* error,
                            size_t error_size) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    if (VPMacOSMetalUploaderCopyCurrentFrameWithLayout(
            uploader, player, buffer, width, height, 0, out, error, error_size) == 0) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool measure_pixel_buffer(CVPixelBufferRef buffer,
                          int width,
                          int height,
                          VPMacOSCaptureMetrics* out) {
  if (!buffer || !out) {
    return false;
  }
  CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  void* base = CVPixelBufferGetBaseAddress(buffer);
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  const int ret = VPMacOSMeasureBGRA(
      static_cast<const uint8_t*>(base),
      width,
      height,
      static_cast<int32_t>(stride),
      out);
  CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  return ret == 0;
}

}  // namespace

int main() {
  VPMacOSMetalUploader* uploader = VPMacOSMetalUploaderCreate();
  if (!uploader) {
    return fail("failed to create native Metal uploader");
  }
  if (VPMacOSMetalUploaderIsAvailable(uploader) == 0) {
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("native Metal uploader is not available");
  }

  CVPixelBufferRef bgra = create_pixel_buffer(kCVPixelFormatType_32BGRA, 64, 32);
  if (!bgra) {
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("failed to create BGRA CVPixelBuffer");
  }
  if (VPMacOSMetalUploaderValidatePixelBuffer(uploader, bgra, 64, 32) == 0) {
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("native Metal uploader rejected valid BGRA CVPixelBuffer");
  }
  if (!expect_validation(
          uploader,
          bgra,
          64,
          32,
          VPMacOSMetalUploaderStatusOk,
          "")) {
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("native Metal uploader checked validation rejected valid BGRA CVPixelBuffer");
  }
  if (VPMacOSMetalUploaderValidatePixelBuffer(uploader, bgra, 63, 32) != 0) {
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("native Metal uploader accepted mismatched pixel-buffer dimensions");
  }
  if (!expect_validation(
          uploader,
          bgra,
          63,
          32,
          VPMacOSMetalUploaderStatusSizeMismatch,
          "native Metal pixel buffer dimensions do not match the presentation surface")) {
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("native Metal uploader did not report size-mismatch validation status");
  }

  CVPixelBufferRef argb = create_pixel_buffer(kCVPixelFormatType_32ARGB, 64, 32);
  if (!argb) {
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("failed to create ARGB CVPixelBuffer");
  }
  if (VPMacOSMetalUploaderValidatePixelBuffer(uploader, argb, 64, 32) != 0) {
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("native Metal uploader accepted non-BGRA CVPixelBuffer");
  }
  if (!expect_validation(
          uploader,
          argb,
          64,
          32,
          VPMacOSMetalUploaderStatusUnsupportedPixelFormat,
          "native Metal pixel buffer must be 32-bit BGRA")) {
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("native Metal uploader did not report unsupported-pixel-format status");
  }

  const std::string path = vp_tools::h264_smoke_video_path(VIDEO_TEST_DIR);
  if (path.empty()) {
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("missing H.264 test video for Metal layout upload smoke");
  }
  VPMacOSNativePlayer* player = VPMacOSNativePlayerCreate();
  if (!player) {
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("failed to create macOS native player for Metal layout smoke");
  }
  char error[512] = {};
  if (VPMacOSNativePlayerOpen(player, path.c_str(), error, sizeof(error)) != 0) {
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(stderr, "native player open failed: %s\n", error);
    return 1;
  }
  const int width = VPMacOSNativePlayerWidth(player);
  const int height = VPMacOSNativePlayerHeight(player);
  CVPixelBufferRef layout_buffer =
      create_pixel_buffer(kCVPixelFormatType_32BGRA, width, height);
  if (!layout_buffer) {
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("failed to create layout BGRA CVPixelBuffer");
  }

  VPMacOSNativeFrameInfo default_info = {};
  if (!copy_frame_with_layout(
          uploader, player, layout_buffer, width, height,
          &default_info, error, sizeof(error))) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(stderr, "default Metal layout upload failed: %s\n", error);
    return 1;
  }
  VPMacOSCaptureMetrics default_metrics = {};
  if (!measure_pixel_buffer(layout_buffer, width, height, &default_metrics) ||
      default_metrics.non_black_ratio <= 0.5) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("default Metal layout upload produced invalid pixels");
  }

  VPMacOSNativeLayoutState zoom_layout = {};
  zoom_layout.mode = 0;
  zoom_layout.split_pos = 0.5f;
  zoom_layout.zoom_ratio = 2.0f;
  zoom_layout.pixel_size_mode = 1;
  zoom_layout.order[0] = 0;
  zoom_layout.order[1] = 1;
  zoom_layout.order[2] = 2;
  zoom_layout.order[3] = 3;
  VPMacOSNativePlayerApplyLayout(player, &zoom_layout);

  VPMacOSNativeFrameInfo zoom_info = {};
  if (!copy_frame_with_layout(
          uploader, player, layout_buffer, width, height,
          &zoom_info, error, sizeof(error))) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(stderr, "zoomed Metal layout upload failed: %s\n", error);
    return 1;
  }
  VPMacOSCaptureMetrics zoom_metrics = {};
  if (!measure_pixel_buffer(layout_buffer, width, height, &zoom_metrics) ||
      zoom_metrics.non_black_ratio <= 0.5 ||
      zoom_metrics.hash == default_metrics.hash ||
      zoom_info.pts_us != default_info.pts_us) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("zoomed Metal layout upload did not transform the current frame");
  }

  CFRelease(layout_buffer);
  VPMacOSNativePlayerDestroy(player);
  CFRelease(argb);
  CFRelease(bgra);
  VPMacOSMetalUploaderDestroy(uploader);
  return 0;
}
