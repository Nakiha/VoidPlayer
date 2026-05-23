#include "macos/native_player_bridge.h"

#include <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>

#include <cstring>
#include <cstdio>

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

  CFRelease(argb);
  CFRelease(bgra);
  VPMacOSMetalUploaderDestroy(uploader);
  return 0;
}
