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
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    if (VPMacOSMetalUploaderCopyCurrentFrameWithLayout(
            uploader,
            player,
            buffer,
            width,
            height,
            VPMacOSNativeMaxTracks,
            0,
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
  const std::filesystem::path second_path =
      std::filesystem::path(VIDEO_TEST_DIR) / "ci_h264_smoke.mp4";
  if (!std::filesystem::is_regular_file(second_path) ||
      vp_tools::is_git_lfs_pointer(second_path)) {
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("missing secondary 320x180 test video for mixed-size Metal layout smoke");
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

  VPMacOSNativeTrackInfo second_track = {};
  if (VPMacOSNativePlayerAddTrack(
          player, second_path.string().c_str(), 1, &second_track, error, sizeof(error)) != 0 ||
      second_track.slot != 1 ||
      second_track.width != 320 ||
      second_track.height != 180) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(stderr, "second native track add failed for Metal layout smoke: %s\n", error);
    return 1;
  }

  VPMacOSNativeFrameInfo multitrack_info = {};
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  const size_t track_bytes = row_bytes * static_cast<size_t>(height);
  std::vector<uint8_t> present_cpu(track_bytes * VPMacOSNativeMaxTracks, 0);
  VPMacOSNativePresentDecisionInfo present_decision = {};
  VPMacOSCaptureMetrics present_slot0_metrics = {};
  VPMacOSCaptureMetrics present_slot1_metrics = {};
  bool present_copy_ready = false;
  const auto present_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < present_deadline) {
    if (VPMacOSNativePlayerCopyPresentFramesBGRAInto(
            player,
            present_cpu.data(),
            present_cpu.size(),
            width,
            height,
            static_cast<int32_t>(row_bytes),
            track_bytes,
            &present_decision,
            error,
            sizeof(error)) == 0 &&
        present_decision.frame_count == 2) {
      present_copy_ready = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!present_copy_ready ||
      VPMacOSMeasureBGRA(
          present_cpu.data(),
          width,
          height,
          static_cast<int32_t>(row_bytes),
          &present_slot0_metrics) != 0 ||
      VPMacOSMeasureBGRA(
          present_cpu.data() + track_bytes,
          second_track.width,
          second_track.height,
          static_cast<int32_t>(row_bytes),
          &present_slot1_metrics) != 0 ||
      present_slot0_metrics.non_black_ratio <= 0.5 ||
      present_slot1_metrics.non_black_ratio <= 0.5 ||
      present_decision.source_width[0] != width ||
      present_decision.source_height[0] != height ||
      present_decision.source_width[1] != second_track.width ||
      present_decision.source_height[1] != second_track.height) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(stderr, "native present decision BGRA copy failed: %s\n", error);
    return 1;
  }

  const size_t package_size =
      VPMacOSNativePresentFramePackageMaxBytes(width, height, VPMacOSNativeMaxTracks);
  std::vector<uint8_t> present_package(package_size, 0);
  VPMacOSNativePresentFramePackageInfo package_info = {};
  bool package_ready = false;
  const auto package_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < package_deadline) {
    if (VPMacOSNativePlayerCopyPresentFramePackage(
            player,
            present_package.data(),
            present_package.size(),
            width,
            height,
            VPMacOSNativeMaxTracks,
            &package_info,
            error,
            sizeof(error)) == 0 &&
        package_info.decision.frame_count == 2) {
      package_ready = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!package_ready ||
      package_info.storage == VPMacOSNativePresentPackageStorageUnavailable ||
      package_info.used_bytes == 0 ||
      package_info.used_bytes > present_package.size() ||
      package_info.decision.source_width[0] != width ||
      package_info.decision.source_width[1] != second_track.width) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(stderr, "native present frame package copy failed: %s\n", error);
    return 1;
  }

  if (!copy_frame_with_layout(
          uploader, player, layout_buffer, width, height,
          &multitrack_info, error, sizeof(error))) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(stderr, "multi-track Metal layout upload failed: %s\n", error);
    return 1;
  }
  VPMacOSCaptureMetrics multitrack_metrics = {};
  const double left_center_non_black = pixel_buffer_rect_non_black_ratio(
      layout_buffer, width / 8, height * 3 / 8, width / 4, height / 4);
  const double right_center_non_black = pixel_buffer_rect_non_black_ratio(
      layout_buffer, width * 5 / 8, height * 3 / 8, width / 4, height / 4);
  if (!measure_pixel_buffer(layout_buffer, width, height, &multitrack_metrics) ||
      multitrack_metrics.non_black_ratio <= 0.24 ||
      left_center_non_black <= 0.8 ||
      right_center_non_black <= 0.05 ||
      multitrack_metrics.hash == default_metrics.hash ||
      multitrack_info.pts_us != default_info.pts_us) {
    std::fprintf(
        stderr,
        "multi-track metrics: default_hash=%llu multi_hash=%llu "
        "default_non_black=%.4f multi_non_black=%.4f "
        "left_center=%.4f right_center=%.4f "
        "default_pts=%lld multi_pts=%lld\n",
        static_cast<unsigned long long>(default_metrics.hash),
        static_cast<unsigned long long>(multitrack_metrics.hash),
        default_metrics.non_black_ratio,
        multitrack_metrics.non_black_ratio,
        left_center_non_black,
        right_center_non_black,
        static_cast<long long>(default_info.pts_us),
        static_cast<long long>(multitrack_info.pts_us));
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("multi-track Metal layout upload did not compose the present decision");
  }

  VPMacOSNativeLayoutState split_layout = {};
  split_layout.mode = 1;
  split_layout.split_pos = 0.0f;
  split_layout.zoom_ratio = 1.0f;
  split_layout.pixel_size_mode = 1;
  split_layout.order[0] = 0;
  split_layout.order[1] = 1;
  split_layout.order[2] = 2;
  split_layout.order[3] = 3;
  VPMacOSNativePlayerApplyLayout(player, &split_layout);

  VPMacOSNativeFrameInfo split_zero_info = {};
  if (!copy_frame_with_layout(
          uploader, player, layout_buffer, width, height,
          &split_zero_info, error, sizeof(error))) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(stderr, "split-zero Metal layout upload failed: %s\n", error);
    return 1;
  }
  VPMacOSCaptureMetrics split_zero_metrics = {};
  if (!measure_pixel_buffer(layout_buffer, width, height, &split_zero_metrics) ||
      split_zero_metrics.non_black_ratio <= 0.5 ||
      split_zero_info.pts_us != default_info.pts_us) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("split-zero Metal layout upload did not render the secondary track");
  }

  split_layout.split_pos = 1.0f;
  VPMacOSNativePlayerApplyLayout(player, &split_layout);
  VPMacOSNativeFrameInfo split_one_info = {};
  if (!copy_frame_with_layout(
          uploader, player, layout_buffer, width, height,
          &split_one_info, error, sizeof(error))) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    std::fprintf(stderr, "split-one Metal layout upload failed: %s\n", error);
    return 1;
  }
  VPMacOSCaptureMetrics split_one_metrics = {};
  if (!measure_pixel_buffer(layout_buffer, width, height, &split_one_metrics) ||
      split_one_metrics.non_black_ratio <= 0.5 ||
      split_one_metrics.hash == split_zero_metrics.hash ||
      split_one_info.pts_us != default_info.pts_us) {
    CFRelease(layout_buffer);
    VPMacOSNativePlayerDestroy(player);
    CFRelease(argb);
    CFRelease(bgra);
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("split-one Metal layout upload did not switch to the primary track");
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
