#include "macos/native_player_bridge.h"
#include "tools/test_video_assets.h"

#include <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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
  const size_t package_size =
      VPMacOSNativePresentFramePackageMaxBytes(width, height, VPMacOSNativeMaxTracks);
  if (package_size == 0) {
    std::snprintf(error, error_size, "%s", "present package dimensions overflow");
    return false;
  }
  std::vector<uint8_t> present_package(package_size, 0);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    VPMacOSNativePresentFramePackageInfo package_info = {};
    if (VPMacOSNativePlayerCopyPresentFramePackage(
            player,
            present_package.data(),
            present_package.size(),
            width,
            height,
            VPMacOSNativeMaxTracks,
            &package_info,
            error,
            error_size) == 0 &&
        VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
            uploader,
            present_package.data(),
            present_package.size(),
            &package_info,
            buffer,
            width,
            height,
            out,
            error,
            error_size) == 0) {
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

double pixel_buffer_rect_non_black_ratio(CVPixelBufferRef buffer,
                                         int x,
                                         int y,
                                         int width,
                                         int height) {
  if (!buffer || x < 0 || y < 0 || width <= 0 || height <= 0) {
    return 0.0;
  }
  CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  const int buffer_width = static_cast<int>(CVPixelBufferGetWidth(buffer));
  const int buffer_height = static_cast<int>(CVPixelBufferGetHeight(buffer));
  const auto* base =
      static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  const int max_x = std::min(buffer_width, x + width);
  const int max_y = std::min(buffer_height, y + height);
  int non_black = 0;
  int pixel_count = 0;
  if (base && x < buffer_width && y < buffer_height) {
    for (int row_y = y; row_y < max_y; ++row_y) {
      const uint8_t* row = base + static_cast<size_t>(row_y) * stride;
      for (int col_x = x; col_x < max_x; ++col_x) {
        const uint8_t b = row[col_x * 4 + 0];
        const uint8_t g = row[col_x * 4 + 1];
        const uint8_t r = row[col_x * 4 + 2];
        if (r > 4 || g > 4 || b > 4) {
          ++non_black;
        }
        ++pixel_count;
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  return pixel_count > 0
      ? static_cast<double>(non_black) / static_cast<double>(pixel_count)
      : 0.0;
}

bool read_pixel_bgra(CVPixelBufferRef buffer,
                     int x,
                     int y,
                     uint8_t* b,
                     uint8_t* g,
                     uint8_t* r,
                     uint8_t* a) {
  if (!buffer || !b || !g || !r || !a || x < 0 || y < 0 ||
      x >= static_cast<int>(CVPixelBufferGetWidth(buffer)) ||
      y >= static_cast<int>(CVPixelBufferGetHeight(buffer))) {
    return false;
  }
  CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  const auto* base =
      static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  if (!base) {
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return false;
  }
  const uint8_t* pixel = base + static_cast<size_t>(y) * stride +
      static_cast<size_t>(x) * 4u;
  *b = pixel[0];
  *g = pixel[1];
  *r = pixel[2];
  *a = pixel[3];
  CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  return true;
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

  CVPixelBufferRef planar_buffer =
      create_pixel_buffer(kCVPixelFormatType_32BGRA, 4, 4);
  if (!planar_buffer) {
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("failed to create planar YUV parity CVPixelBuffer");
  }
  std::vector<uint8_t> planar_package_data(24, 128);
  VPMacOSNativePresentFramePackageInfo planar_package = {};
  planar_package.storage = VPMacOSNativePresentPackageStorageYUV;
  planar_package.width = 4;
  planar_package.height = 4;
  planar_package.max_track_slots = 1;
  planar_package.used_bytes = planar_package_data.size();
  auto& planar_decision = planar_package.decision;
  planar_decision.should_present = 1;
  planar_decision.frame_count = 1;
  planar_decision.track_count = 1;
  planar_decision.mode = 0;
  planar_decision.split_pos = 0.5f;
  planar_decision.order[0] = 0;
  planar_decision.display_offset_x[0] = 0.0f;
  planar_decision.display_offset_y[0] = 0.0f;
  planar_decision.inv_display_size_x[0] = 1.0f;
  planar_decision.inv_display_size_y[0] = 1.0f;
  planar_decision.source_width[0] = 4;
  planar_decision.source_height[0] = 4;
  planar_decision.yuv_format[0] = VPMacOSNativePresentFormatYUV420P;
  planar_decision.y_offset[0] = 0;
  planar_decision.uv_offset[0] = 16;
  planar_decision.v_offset[0] = 20;
  planar_decision.y_stride[0] = 4;
  planar_decision.uv_stride[0] = 2;
  planar_decision.coded_width[0] = 4;
  planar_decision.coded_height[0] = 4;
  planar_decision.nv12_uv_scale_x[0] = 1.0f;
  planar_decision.nv12_uv_scale_y[0] = 1.0f;
  planar_decision.color_range[0] = 2;
  planar_decision.color_matrix[0] = 2;
  planar_decision.frames[0].present = 1;
  planar_decision.frames[0].slot = 0;
  planar_decision.frames[0].width = 4;
  planar_decision.frames[0].height = 4;
  planar_decision.frames[0].pts_us = 42;
  char planar_error[512] = {};
  VPMacOSNativeFrameInfo planar_info = {};
  const int64_t planar_upload_count_before =
      VPMacOSMetalUploaderPresentPackageUploadCount(uploader);
  if (VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
          uploader,
          planar_package_data.data(),
          planar_package_data.size(),
          &planar_package,
          planar_buffer,
          4,
          4,
          &planar_info,
          planar_error,
          sizeof(planar_error)) != 0) {
    CFRelease(planar_buffer);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(stderr, "planar YUV420 Metal package upload failed: %s\n", planar_error);
    return 1;
  }
  uint8_t b = 0;
  uint8_t g = 0;
  uint8_t r = 0;
  uint8_t a = 0;
  if (!read_pixel_bgra(planar_buffer, 2, 2, &b, &g, &r, &a) ||
      r < 120 || r > 136 ||
      g < 120 || g > 136 ||
      b < 120 || b > 136 ||
      a != 255 ||
      planar_info.pts_us != 42 ||
      VPMacOSMetalUploaderPresentPackageUploadCount(uploader) <=
          planar_upload_count_before ||
      VPMacOSMetalUploaderLastPresentPackageStorage(uploader) !=
          VPMacOSNativePresentPackageStorageYUV) {
    CFRelease(planar_buffer);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(
        stderr,
        "planar YUV420 parity pixel BGRA=(%u,%u,%u,%u), pts=%lld\n",
        static_cast<unsigned>(b),
        static_cast<unsigned>(g),
        static_cast<unsigned>(r),
        static_cast<unsigned>(a),
        static_cast<long long>(planar_info.pts_us));
    return fail("planar YUV420 Metal package upload did not produce neutral BGRA");
  }
  CFRelease(planar_buffer);

  CVPixelBufferRef nv12_buffer =
      create_pixel_buffer(kCVPixelFormatType_32BGRA, 4, 4);
  if (!nv12_buffer) {
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("failed to create NV12 parity CVPixelBuffer");
  }
  std::vector<uint8_t> nv12_package_data(24, 128);
  VPMacOSNativePresentFramePackageInfo nv12_package = {};
  nv12_package.storage = VPMacOSNativePresentPackageStorageYUV;
  nv12_package.width = 4;
  nv12_package.height = 4;
  nv12_package.max_track_slots = 1;
  nv12_package.used_bytes = nv12_package_data.size();
  auto& nv12_decision = nv12_package.decision;
  nv12_decision.should_present = 1;
  nv12_decision.frame_count = 1;
  nv12_decision.track_count = 1;
  nv12_decision.mode = 0;
  nv12_decision.split_pos = 0.5f;
  nv12_decision.order[0] = 0;
  nv12_decision.display_offset_x[0] = 0.0f;
  nv12_decision.display_offset_y[0] = 0.0f;
  nv12_decision.inv_display_size_x[0] = 1.0f;
  nv12_decision.inv_display_size_y[0] = 1.0f;
  nv12_decision.source_width[0] = 4;
  nv12_decision.source_height[0] = 4;
  nv12_decision.yuv_format[0] = VPMacOSNativePresentFormatNV12;
  nv12_decision.y_offset[0] = 0;
  nv12_decision.uv_offset[0] = 16;
  nv12_decision.y_stride[0] = 4;
  nv12_decision.uv_stride[0] = 4;
  nv12_decision.coded_width[0] = 4;
  nv12_decision.coded_height[0] = 4;
  nv12_decision.nv12_uv_scale_x[0] = 1.0f;
  nv12_decision.nv12_uv_scale_y[0] = 1.0f;
  nv12_decision.color_range[0] = 2;
  nv12_decision.color_matrix[0] = 2;
  nv12_decision.frames[0].present = 1;
  nv12_decision.frames[0].slot = 0;
  nv12_decision.frames[0].width = 4;
  nv12_decision.frames[0].height = 4;
  nv12_decision.frames[0].pts_us = 43;
  char nv12_error[512] = {};
  VPMacOSNativeFrameInfo nv12_info = {};
  if (VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
          uploader,
          nv12_package_data.data(),
          nv12_package_data.size(),
          &nv12_package,
          nv12_buffer,
          4,
          4,
          &nv12_info,
          nv12_error,
          sizeof(nv12_error)) != 0 ||
      !read_pixel_bgra(nv12_buffer, 2, 2, &b, &g, &r, &a) ||
      r < 120 || r > 136 ||
      g < 120 || g > 136 ||
      b < 120 || b > 136 ||
      a != 255 ||
      nv12_info.pts_us != 43) {
    CFRelease(nv12_buffer);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(
        stderr,
        "NV12 parity failed: error=%s BGRA=(%u,%u,%u,%u), pts=%lld\n",
        nv12_error,
        static_cast<unsigned>(b),
        static_cast<unsigned>(g),
        static_cast<unsigned>(r),
        static_cast<unsigned>(a),
        static_cast<long long>(nv12_info.pts_us));
    return fail("NV12 Metal package upload did not produce neutral BGRA");
  }
  CFRelease(nv12_buffer);

  CVPixelBufferRef p010_buffer =
      create_pixel_buffer(kCVPixelFormatType_32BGRA, 4, 4);
  if (!p010_buffer) {
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("failed to create P010 parity CVPixelBuffer");
  }
  std::vector<uint8_t> p010_package_data(48, 0);
  const uint16_t neutral_p010 = static_cast<uint16_t>(512u << 6);
  for (size_t offset = 0; offset + 1 < p010_package_data.size(); offset += 2) {
    p010_package_data[offset] = static_cast<uint8_t>(neutral_p010 & 0xffu);
    p010_package_data[offset + 1] = static_cast<uint8_t>(neutral_p010 >> 8);
  }
  VPMacOSNativePresentFramePackageInfo p010_package = {};
  p010_package.storage = VPMacOSNativePresentPackageStorageYUV;
  p010_package.width = 4;
  p010_package.height = 4;
  p010_package.max_track_slots = 1;
  p010_package.used_bytes = p010_package_data.size();
  auto& p010_decision = p010_package.decision;
  p010_decision.should_present = 1;
  p010_decision.frame_count = 1;
  p010_decision.track_count = 1;
  p010_decision.mode = 0;
  p010_decision.split_pos = 0.5f;
  p010_decision.order[0] = 0;
  p010_decision.display_offset_x[0] = 0.0f;
  p010_decision.display_offset_y[0] = 0.0f;
  p010_decision.inv_display_size_x[0] = 1.0f;
  p010_decision.inv_display_size_y[0] = 1.0f;
  p010_decision.source_width[0] = 4;
  p010_decision.source_height[0] = 4;
  p010_decision.yuv_format[0] = VPMacOSNativePresentFormatP010;
  p010_decision.y_offset[0] = 0;
  p010_decision.uv_offset[0] = 32;
  p010_decision.y_stride[0] = 8;
  p010_decision.uv_stride[0] = 8;
  p010_decision.coded_width[0] = 4;
  p010_decision.coded_height[0] = 4;
  p010_decision.nv12_uv_scale_x[0] = 1.0f;
  p010_decision.nv12_uv_scale_y[0] = 1.0f;
  p010_decision.color_range[0] = 2;
  p010_decision.color_matrix[0] = 2;
  p010_decision.frames[0].present = 1;
  p010_decision.frames[0].slot = 0;
  p010_decision.frames[0].width = 4;
  p010_decision.frames[0].height = 4;
  p010_decision.frames[0].pts_us = 44;
  char p010_error[512] = {};
  VPMacOSNativeFrameInfo p010_info = {};
  if (VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
          uploader,
          p010_package_data.data(),
          p010_package_data.size(),
          &p010_package,
          p010_buffer,
          4,
          4,
          &p010_info,
          p010_error,
          sizeof(p010_error)) != 0 ||
      !read_pixel_bgra(p010_buffer, 2, 2, &b, &g, &r, &a) ||
      r < 120 || r > 136 ||
      g < 120 || g > 136 ||
      b < 120 || b > 136 ||
      a != 255 ||
      p010_info.pts_us != 44) {
    CFRelease(p010_buffer);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(
        stderr,
        "P010 parity failed: error=%s BGRA=(%u,%u,%u,%u), pts=%lld\n",
        p010_error,
        static_cast<unsigned>(b),
        static_cast<unsigned>(g),
        static_cast<unsigned>(r),
        static_cast<unsigned>(a),
        static_cast<long long>(p010_info.pts_us));
    return fail("P010 Metal package upload did not produce neutral BGRA");
  }
  CFRelease(p010_buffer);

  CFRelease(argb);
  CFRelease(bgra);
  VPMacOSMetalUploaderDestroy(uploader);
  return 0;
}
