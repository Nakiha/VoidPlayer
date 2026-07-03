#include "macos/player/native_player_bridge.h"

#include <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

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

[[maybe_unused]] bool measure_pixel_buffer(CVPixelBufferRef buffer,
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

[[maybe_unused]] double pixel_buffer_rect_non_black_ratio(CVPixelBufferRef buffer,
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

struct AsyncUploadContext {
  std::mutex mutex;
  std::condition_variable cv;
  bool completed = false;
  int ret = 999;
  VPMacOSNativeFrameInfo frame_info = {};
  std::string error;
};

void async_upload_completed(void* user_data,
                            int ret,
                            VPMacOSNativeFrameInfo frame_info,
                            const char* error,
                            int64_t,
                            int64_t) {
  auto* context = static_cast<AsyncUploadContext*>(user_data);
  std::lock_guard<std::mutex> lock(context->mutex);
  context->completed = true;
  context->ret = ret;
  context->frame_info = frame_info;
  context->error = error ? error : "";
  context->cv.notify_all();
}

bool wait_for_async_upload(AsyncUploadContext& context) {
  std::unique_lock<std::mutex> lock(context.mutex);
  return context.cv.wait_for(lock, std::chrono::seconds(5), [&] {
    return context.completed;
  });
}

VPMacOSNativePresentFramePackageInfo make_nv12_package(int width,
                                                       int height,
                                                       int64_t pts_us,
                                                       size_t* out_size) {
  const size_t y_size = static_cast<size_t>(width) * height;
  const size_t uv_size = static_cast<size_t>(width) * (height / 2);
  if (out_size) {
    *out_size = y_size + uv_size;
  }
  VPMacOSNativePresentFramePackageInfo package = {};
  package.storage = VPMacOSNativePresentPackageStorageYUV;
  package.width = width;
  package.height = height;
  package.max_track_slots = 1;
  package.used_bytes = y_size + uv_size;
  auto& decision = package.decision;
  decision.should_present = 1;
  decision.frame_count = 1;
  decision.track_count = 1;
  decision.mode = 0;
  decision.split_pos = 0.5f;
  decision.order[0] = 0;
  decision.display_offset_x[0] = 0.0f;
  decision.display_offset_y[0] = 0.0f;
  decision.inv_display_size_x[0] = 1.0f;
  decision.inv_display_size_y[0] = 1.0f;
  decision.source_width[0] = width;
  decision.source_height[0] = height;
  decision.yuv_format[0] = VPMacOSNativePresentFormatNV12;
  decision.y_offset[0] = 0;
  decision.uv_offset[0] = static_cast<int64_t>(y_size);
  decision.y_stride[0] = width;
  decision.uv_stride[0] = width;
  decision.coded_width[0] = width;
  decision.coded_height[0] = height;
  decision.nv12_uv_scale_x[0] = 1.0f;
  decision.nv12_uv_scale_y[0] = 1.0f;
  decision.color_range[0] = 2;
  decision.color_matrix[0] = 2;
  decision.frames[0].present = 1;
  decision.frames[0].slot = 0;
  decision.frames[0].width = width;
  decision.frames[0].height = height;
  decision.frames[0].pts_us = pts_us;
  return package;
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
          "native Metal pixel buffer must be 32-bit BGRA or 64-bit RGBA half")) {
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("native Metal uploader did not report unsupported-pixel-format status");
  }

  CVPixelBufferRef rgba_half =
      create_pixel_buffer(kCVPixelFormatType_64RGBAHalf, 64, 32);
  if (!rgba_half) {
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("failed to create RGBA half CVPixelBuffer");
  }
  if (!expect_validation(
          uploader,
          rgba_half,
          64,
          32,
          VPMacOSMetalUploaderStatusOk,
          "")) {
    CFRelease(rgba_half);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("native Metal uploader rejected valid RGBA half CVPixelBuffer");
  }
  CFRelease(rgba_half);

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
  char prepared_error[512] = {};
  VPMacOSNativeFrameInfo prepared_info = {};
  if (VPMacOSMetalUploaderUploadPreparedPresentFramePackageWithLayoutAndOverlay(
          uploader,
          &planar_package,
          nullptr,
          planar_buffer,
          4,
          4,
          &prepared_info,
          prepared_error,
          sizeof(prepared_error)) != -1 ||
      std::strcmp(
          prepared_error,
          "prepared native Metal package upload is disabled; pass package bytes explicitly") !=
          0) {
    CFRelease(planar_buffer);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(stderr, "prepared Metal upload was not rejected: %s\n", prepared_error);
    return 1;
  }
  char prepared_async_error[512] = {};
  if (VPMacOSMetalUploaderUploadPreparedPresentFramePackageWithLayoutAndOverlayAsync(
          uploader,
          &planar_package,
          nullptr,
          planar_buffer,
          4,
          4,
          &prepared_info,
          prepared_async_error,
          sizeof(prepared_async_error),
          async_upload_completed,
          nullptr) != -1 ||
      std::strcmp(
          prepared_async_error,
          "prepared native Metal package upload is disabled; pass package bytes explicitly") !=
          0) {
    CFRelease(planar_buffer);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(
        stderr,
        "prepared async Metal upload was not rejected: %s\n",
        prepared_async_error);
    return 1;
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

  constexpr int async_width = 2048;
  constexpr int async_height = 2048;
  size_t async_data_size = 0;
  VPMacOSNativePresentFramePackageInfo async_package =
      make_nv12_package(async_width, async_height, 1001, &async_data_size);
  std::vector<uint8_t> async_package_data(async_data_size, 128);
  constexpr size_t async_pool_submissions = 3;
  std::array<CVPixelBufferRef, async_pool_submissions + 1> async_buffers = {};
  for (auto& buffer : async_buffers) {
    buffer = create_pixel_buffer(kCVPixelFormatType_32BGRA, async_width, async_height);
  }
  const auto release_async_buffers = [&] {
    for (auto* buffer : async_buffers) {
      if (buffer) {
        CFRelease(buffer);
      }
    }
  };
  if (std::any_of(async_buffers.begin(), async_buffers.end(), [](CVPixelBufferRef buffer) {
        return buffer == nullptr;
      })) {
    release_async_buffers();
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("failed to create async Metal upload CVPixelBuffers");
  }

  std::array<AsyncUploadContext, async_pool_submissions> async_contexts = {};
  for (size_t i = 0; i < async_pool_submissions; ++i) {
    async_package.decision.frames[0].pts_us = 1001 + static_cast<int64_t>(i);
    VPMacOSNativeFrameInfo async_info = {};
    char async_error[512] = {};
    const int async_ret =
        VPMacOSMetalUploaderCopyPresentFramePackageWithLayoutAndOverlayAsync(
            uploader,
            async_package_data.data(),
            async_package_data.size(),
            &async_package,
            nullptr,
            async_buffers[i],
            async_width,
            async_height,
            &async_info,
            async_error,
            sizeof(async_error),
            async_upload_completed,
            &async_contexts[i]);
    if (async_ret != 0) {
      for (size_t wait = 0; wait < i; ++wait) {
        wait_for_async_upload(async_contexts[wait]);
      }
      release_async_buffers();
      CFRelease(argb);
      CFRelease(bgra);
      VPMacOSMetalUploaderDestroy(uploader);
      std::fprintf(stderr,
                   "async Metal upload %zu failed to submit: %s\n",
                   i,
                   async_error);
      return 1;
    }
  }

  async_package.decision.frames[0].pts_us = 2001;
  VPMacOSNativeFrameInfo async_info_busy = {};
  char async_error_busy[512] = {};
  AsyncUploadContext async_context_busy;
  const int async_ret_busy =
      VPMacOSMetalUploaderCopyPresentFramePackageWithLayoutAndOverlayAsync(
          uploader,
          async_package_data.data(),
          async_package_data.size(),
          &async_package,
          nullptr,
          async_buffers.back(),
          async_width,
          async_height,
          &async_info_busy,
          async_error_busy,
          sizeof(async_error_busy),
          async_upload_completed,
          &async_context_busy);
  const bool async_busy_rejected =
      async_ret_busy == -2 &&
      std::strcmp(async_error_busy, "native Metal uploader frame resource pool is busy") == 0;
  const bool async_busy_accepted = async_ret_busy == 0;
  if (!async_busy_rejected && !async_busy_accepted) {
    for (auto& context : async_contexts) {
      wait_for_async_upload(context);
    }
    release_async_buffers();
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(
        stderr,
        "async Metal upload beyond resource pool did not report busy resources: ret=%d error=%s\n",
        async_ret_busy,
        async_error_busy);
    return 1;
  }

  for (size_t i = 0; i < async_pool_submissions; ++i) {
    if (!wait_for_async_upload(async_contexts[i]) ||
        async_contexts[i].ret != 0 ||
        async_contexts[i].frame_info.pts_us !=
            1001 + static_cast<int64_t>(i)) {
      release_async_buffers();
      CFRelease(argb);
      CFRelease(bgra);
      VPMacOSMetalUploaderDestroy(uploader);
      std::fprintf(
          stderr,
          "async Metal upload %zu did not complete cleanly: ret=%d error=%s pts=%lld\n",
          i,
          async_contexts[i].ret,
          async_contexts[i].error.c_str(),
          static_cast<long long>(async_contexts[i].frame_info.pts_us));
      return 1;
    }
  }

  if (async_busy_accepted &&
      (!wait_for_async_upload(async_context_busy) ||
       async_context_busy.ret != 0 ||
       async_context_busy.frame_info.pts_us != 2001)) {
    release_async_buffers();
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(
        stderr,
        "async Metal upload accepted after resource reuse did not complete cleanly: ret=%d error=%s pts=%lld\n",
        async_context_busy.ret,
        async_context_busy.error.c_str(),
        static_cast<long long>(async_context_busy.frame_info.pts_us));
    return 1;
  }

  async_package.decision.frames[0].pts_us = 3001;
  AsyncUploadContext async_context_reused;
  VPMacOSNativeFrameInfo async_info_reused = {};
  char async_error_reused[512] = {};
  const int async_ret_reused =
      VPMacOSMetalUploaderCopyPresentFramePackageWithLayoutAndOverlayAsync(
          uploader,
          async_package_data.data(),
          async_package_data.size(),
          &async_package,
          nullptr,
          async_buffers[0],
          async_width,
          async_height,
          &async_info_reused,
          async_error_reused,
          sizeof(async_error_reused),
          async_upload_completed,
          &async_context_reused);
  if (async_ret_reused != 0 ||
      !wait_for_async_upload(async_context_reused) ||
      async_context_reused.ret != 0 ||
      async_context_reused.frame_info.pts_us != 3001) {
    release_async_buffers();
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(
        stderr,
        "async Metal uploader did not accept work after pool completion: submit=%d ret=%d error=%s pts=%lld\n",
        async_ret_reused,
        async_context_reused.ret,
        async_context_reused.error.c_str(),
        static_cast<long long>(async_context_reused.frame_info.pts_us));
    return 1;
  }
  release_async_buffers();

  CFRelease(argb);
  CFRelease(bgra);
  VPMacOSMetalUploaderDestroy(uploader);
  return 0;
}
