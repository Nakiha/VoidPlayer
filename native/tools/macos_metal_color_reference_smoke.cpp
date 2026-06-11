#include "macos/player/native_player_bridge.h"
#include "renderer/color/color_reference.h"
#include "renderer/frame/frame_storage.h"

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 4;
constexpr int kHeight = 4;

int fail(const char* message) {
  std::fprintf(stderr, "%s\n", message);
  return 1;
}

struct ScopedPixelBuffer {
  CVPixelBufferRef buffer = nullptr;

  ~ScopedPixelBuffer() {
    if (buffer) {
      CVPixelBufferRelease(buffer);
    }
  }

  ScopedPixelBuffer(const ScopedPixelBuffer&) = delete;
  ScopedPixelBuffer& operator=(const ScopedPixelBuffer&) = delete;
  ScopedPixelBuffer() = default;
  ScopedPixelBuffer(ScopedPixelBuffer&& other) noexcept
      : buffer(other.buffer) {
    other.buffer = nullptr;
  }
  ScopedPixelBuffer& operator=(ScopedPixelBuffer&& other) noexcept {
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

ScopedPixelBuffer create_pixel_buffer(OSType pixel_format,
                                      int width,
                                      int height,
                                      bool iosurface = true) {
  ScopedPixelBuffer holder;
  CFDictionaryRef io_surface_properties = CFDictionaryCreate(
      kCFAllocatorDefault,
      nullptr,
      nullptr,
      0,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  const void* keys[2] = {kCVPixelBufferMetalCompatibilityKey, nullptr};
  const void* values[2] = {kCFBooleanTrue, nullptr};
  int count = 1;
  if (iosurface) {
    keys[count] = kCVPixelBufferIOSurfacePropertiesKey;
    values[count] = io_surface_properties;
    ++count;
  }
  CFDictionaryRef attrs = CFDictionaryCreate(
      kCFAllocatorDefault,
      keys,
      values,
      count,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CVPixelBufferCreate(kCFAllocatorDefault,
                      width,
                      height,
                      pixel_format,
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

float half_to_float(uint16_t half) {
  const double sign = (half & 0x8000u) != 0 ? -1.0 : 1.0;
  const int exponent = static_cast<int>((half >> 10u) & 0x1fu);
  const int mantissa = static_cast<int>(half & 0x03ffu);
  if (exponent == 0) {
    if (mantissa == 0) {
      return static_cast<float>(sign * 0.0);
    } else {
      return static_cast<float>(sign * std::ldexp(static_cast<double>(mantissa), -24));
    }
  } else if (exponent == 0x1fu) {
    if (mantissa == 0) {
      return static_cast<float>(sign * std::numeric_limits<double>::infinity());
    }
    return std::numeric_limits<float>::quiet_NaN();
  }
  const double value = 1.0 + static_cast<double>(mantissa) / 1024.0;
  return static_cast<float>(sign * std::ldexp(value, exponent - 15));
}

struct Rgba {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
  double a = 0.0;
};

bool read_rgba_half(CVPixelBufferRef buffer, int x, int y, Rgba* out) {
  if (!buffer || !out ||
      CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) !=
          kCVReturnSuccess) {
    return false;
  }
  const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
  const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
  if (!base) {
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return false;
  }
  const auto* pixel = reinterpret_cast<const uint16_t*>(
      base + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 8u);
  *out = {
      half_to_float(pixel[0]),
      half_to_float(pixel[1]),
      half_to_float(pixel[2]),
      half_to_float(pixel[3]),
  };
  CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
  return true;
}

uint32_t limited_code_from_luma(double y_full, int bit_depth) {
  const double normalized = (y_full * 219.0 + 16.0) / 255.0;
  const uint32_t max_value = (1u << bit_depth) - 1u;
  return static_cast<uint32_t>(std::lround(normalized * max_value));
}

uint32_t neutral_chroma_code(int bit_depth) {
  const uint32_t max_value = (1u << bit_depth) - 1u;
  return static_cast<uint32_t>(std::lround((128.0 / 255.0) * max_value));
}

uint32_t p010_storage_code(int code) {
  return static_cast<uint32_t>(code) << 6u;
}

void write_le16(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
  data[offset] = static_cast<uint8_t>(value & 0xffu);
  data[offset + 1] = static_cast<uint8_t>(value >> 8u);
}

void write_cv_pixel_buffer(CVPixelBufferRef buffer,
                           int y_code,
                           int u_code,
                           int v_code,
                           bool p010) {
  if (!buffer ||
      CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess) {
    return;
  }
  auto* y_plane =
      static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
  auto* uv_plane =
      static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
  const size_t y_stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
  const size_t uv_stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
  const int width = static_cast<int>(CVPixelBufferGetWidthOfPlane(buffer, 0));
  const int height = static_cast<int>(CVPixelBufferGetHeightOfPlane(buffer, 0));
  const int uv_width = static_cast<int>(CVPixelBufferGetWidthOfPlane(buffer, 1));
  const int uv_height = static_cast<int>(CVPixelBufferGetHeightOfPlane(buffer, 1));
  if (y_plane && uv_plane) {
    if (p010) {
      const uint16_t y_value = static_cast<uint16_t>(y_code << 6u);
      const uint16_t u_value = static_cast<uint16_t>(u_code << 6u);
      const uint16_t v_value = static_cast<uint16_t>(v_code << 6u);
      for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
          auto* sample = reinterpret_cast<uint16_t*>(
              y_plane + static_cast<size_t>(y) * y_stride +
              static_cast<size_t>(x) * 2u);
          *sample = y_value;
        }
      }
      for (int y = 0; y < uv_height; ++y) {
        for (int x = 0; x < uv_width; ++x) {
          auto* sample = reinterpret_cast<uint16_t*>(
              uv_plane + static_cast<size_t>(y) * uv_stride +
              static_cast<size_t>(x) * 4u);
          sample[0] = u_value;
          sample[1] = v_value;
        }
      }
    } else {
      for (int y = 0; y < height; ++y) {
        std::fill_n(y_plane + static_cast<size_t>(y) * y_stride,
                    width,
                    static_cast<uint8_t>(y_code));
      }
      for (int y = 0; y < uv_height; ++y) {
        uint8_t* row = uv_plane + static_cast<size_t>(y) * uv_stride;
        for (int x = 0; x < uv_width; ++x) {
          row[x * 2 + 0] = static_cast<uint8_t>(u_code);
          row[x * 2 + 1] = static_cast<uint8_t>(v_code);
        }
      }
    }
  }
  CVPixelBufferUnlockBaseAddress(buffer, 0);
}

struct PackageData {
  VPMacOSNativePresentFramePackageInfo package = {};
  std::vector<uint8_t> bytes;
};

PackageData make_package(int y_code,
                         int u_code,
                         int v_code,
                         int bit_depth,
                         int yuv_format,
                         const vr::ColorReferenceConfig& config) {
  PackageData result;
  const bool p010 = yuv_format == VPMacOSNativePresentFormatP010;
  const int bytes_per_sample = p010 ? 2 : 1;
  const size_t y_stride = static_cast<size_t>(kWidth) * bytes_per_sample;
  const size_t uv_stride = static_cast<size_t>(kWidth) * bytes_per_sample;
  const size_t y_size = y_stride * kHeight;
  const size_t uv_size = uv_stride * (kHeight / 2);
  result.bytes.assign(y_size + uv_size, 0);
  if (p010) {
    const uint16_t y_value = static_cast<uint16_t>(y_code << 6u);
    const uint16_t u_value = static_cast<uint16_t>(u_code << 6u);
    const uint16_t v_value = static_cast<uint16_t>(v_code << 6u);
    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        write_le16(result.bytes,
                   static_cast<size_t>(y) * y_stride + static_cast<size_t>(x) * 2u,
                   y_value);
      }
    }
    for (int y = 0; y < kHeight / 2; ++y) {
      for (int x = 0; x < kWidth / 2; ++x) {
        const size_t offset = y_size + static_cast<size_t>(y) * uv_stride +
            static_cast<size_t>(x) * 4u;
        write_le16(result.bytes, offset, u_value);
        write_le16(result.bytes, offset + 2u, v_value);
      }
    }
  } else {
    std::fill(result.bytes.begin(), result.bytes.begin() + y_size,
              static_cast<uint8_t>(y_code));
    for (int y = 0; y < kHeight / 2; ++y) {
      for (int x = 0; x < kWidth / 2; ++x) {
        const size_t offset = y_size + static_cast<size_t>(y) * uv_stride +
            static_cast<size_t>(x) * 2u;
        result.bytes[offset] = static_cast<uint8_t>(u_code);
        result.bytes[offset + 1u] = static_cast<uint8_t>(v_code);
      }
    }
  }

  auto& package = result.package;
  package.storage = VPMacOSNativePresentPackageStorageYUV;
  package.width = kWidth;
  package.height = kHeight;
  package.max_track_slots = 1;
  package.used_bytes = result.bytes.size();
  auto& decision = package.decision;
  decision.should_present = 1;
  decision.frame_count = 1;
  decision.track_count = 1;
  decision.order[0] = 0;
  decision.display_offset_x[0] = 0.0f;
  decision.display_offset_y[0] = 0.0f;
  decision.inv_display_size_x[0] = 1.0f;
  decision.inv_display_size_y[0] = 1.0f;
  decision.source_width[0] = kWidth;
  decision.source_height[0] = kHeight;
  decision.yuv_format[0] = yuv_format;
  decision.y_offset[0] = 0;
  decision.uv_offset[0] = static_cast<int32_t>(y_size);
  decision.y_stride[0] = static_cast<int32_t>(y_stride);
  decision.uv_stride[0] = static_cast<int32_t>(uv_stride);
  decision.coded_width[0] = kWidth;
  decision.coded_height[0] = kHeight;
  decision.nv12_uv_scale_x[0] = 1.0f;
  decision.nv12_uv_scale_y[0] = 1.0f;
  decision.color_range[0] = config.range;
  decision.color_matrix[0] = config.matrix;
  decision.color_transfer[0] = config.transfer;
  decision.color_primaries[0] = config.primaries;
  decision.frames[0].present = 1;
  decision.frames[0].slot = 0;
  decision.frames[0].width = kWidth;
  decision.frames[0].height = kHeight;
  decision.frames[0].pts_us = 1000 + bit_depth + yuv_format;
  return result;
}

VPMacOSNativePresentDecisionInfo make_decision(
    int yuv_format,
    const vr::ColorReferenceConfig& config) {
  VPMacOSNativePresentDecisionInfo decision = {};
  decision.should_present = 1;
  decision.frame_count = 1;
  decision.track_count = 1;
  decision.order[0] = 0;
  decision.display_offset_x[0] = 0.0f;
  decision.display_offset_y[0] = 0.0f;
  decision.inv_display_size_x[0] = 1.0f;
  decision.inv_display_size_y[0] = 1.0f;
  decision.source_width[0] = kWidth;
  decision.source_height[0] = kHeight;
  decision.yuv_format[0] = yuv_format;
  decision.y_offset[0] = 0;
  decision.uv_offset[0] = 0;
  decision.y_stride[0] = kWidth * (yuv_format == VPMacOSNativePresentFormatP010 ? 2 : 1);
  decision.uv_stride[0] = kWidth * (yuv_format == VPMacOSNativePresentFormatP010 ? 2 : 1);
  decision.coded_width[0] = kWidth;
  decision.coded_height[0] = kHeight;
  decision.nv12_uv_scale_x[0] = 1.0f;
  decision.nv12_uv_scale_y[0] = 1.0f;
  decision.color_range[0] = config.range;
  decision.color_matrix[0] = config.matrix;
  decision.color_transfer[0] = config.transfer;
  decision.color_primaries[0] = config.primaries;
  decision.frames[0].present = 1;
  decision.frames[0].slot = 0;
  decision.frames[0].width = kWidth;
  decision.frames[0].height = kHeight;
  decision.frames[0].pts_us = 2000 + yuv_format;
  return decision;
}

struct Case {
  const char* name = "";
  int y_code = 0;
  int u_code = 0;
  int v_code = 0;
  int bit_depth = 8;
  int yuv_format = VPMacOSNativePresentFormatNV12;
  vr::ColorReferenceConfig config;
  double tolerance = 0.01;
};

bool compare_case(VPMacOSMetalUploader* uploader, const Case& test_case) {
  const auto expected_config = [&] {
    auto copy = test_case.config;
    copy.output_edr = true;
    return copy;
  }();
  const vr::ColorReferenceYuv sample = {
      vr::color_reference_unorm_to_float(
          static_cast<uint32_t>(test_case.y_code), test_case.bit_depth),
      vr::color_reference_unorm_to_float(
          static_cast<uint32_t>(test_case.u_code), test_case.bit_depth),
      vr::color_reference_unorm_to_float(
          static_cast<uint32_t>(test_case.v_code), test_case.bit_depth),
  };
  const auto expected = vr::color_reference_sample_yuv(sample, expected_config);
  const bool p010 = test_case.yuv_format == VPMacOSNativePresentFormatP010;
  const vr::ColorReferenceYuv cv_sample = p010
      ? vr::ColorReferenceYuv{
            vr::color_reference_unorm_to_float(
                p010_storage_code(test_case.y_code), 16),
            vr::color_reference_unorm_to_float(
                p010_storage_code(test_case.u_code), 16),
            vr::color_reference_unorm_to_float(
                p010_storage_code(test_case.v_code), 16),
        }
      : sample;
  const auto cv_expected =
      vr::color_reference_sample_yuv(cv_sample, expected_config);

  auto target = create_pixel_buffer(kCVPixelFormatType_64RGBAHalf, kWidth, kHeight);
  if (!target.buffer) {
    std::fprintf(stderr, "%s failed: could not create RGBA half target\n", test_case.name);
    return false;
  }
  const auto package = make_package(test_case.y_code,
                                    test_case.u_code,
                                    test_case.v_code,
                                    test_case.bit_depth,
                                    test_case.yuv_format,
                                    test_case.config);
  VPMacOSNativeFrameInfo frame_info = {};
  char error[512] = {};
  const int ret = VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
      uploader,
      package.bytes.data(),
      package.bytes.size(),
      &package.package,
      target.buffer,
      kWidth,
      kHeight,
      &frame_info,
      error,
      sizeof(error));
  if (ret != 0) {
    std::fprintf(stderr, "%s package failed: Metal upload error=%s\n", test_case.name, error);
    return false;
  }

  Rgba actual;
  if (!read_rgba_half(target.buffer, 2, 2, &actual)) {
    std::fprintf(stderr, "%s package failed: could not read RGBA half target\n", test_case.name);
    return false;
  }
  const double dr = std::abs(actual.r - expected.r);
  const double dg = std::abs(actual.g - expected.g);
  const double db = std::abs(actual.b - expected.b);
  const double da = std::abs(actual.a - 1.0);
  if (dr > test_case.tolerance || dg > test_case.tolerance ||
      db > test_case.tolerance || da > test_case.tolerance) {
    std::fprintf(stderr,
                 "%s failed: actual=(%.6f, %.6f, %.6f, %.6f) "
                 "expected=(%.6f, %.6f, %.6f, 1.000000) "
                 "diff=(%.6f, %.6f, %.6f, %.6f) tolerance=%.6f\n",
                 test_case.name,
                 actual.r,
                 actual.g,
                 actual.b,
                 actual.a,
                 expected.r,
                 expected.g,
                 expected.b,
                 dr,
                 dg,
                 db,
                 da,
                 test_case.tolerance);
    return false;
  }

  auto cv_target = create_pixel_buffer(kCVPixelFormatType_64RGBAHalf, kWidth, kHeight);
  auto source = create_pixel_buffer(
      p010 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
           : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
      kWidth,
      kHeight);
  if (!cv_target.buffer || !source.buffer) {
    std::fprintf(stderr, "%s cvpixelbuffer failed: could not create buffers\n",
                 test_case.name);
    return false;
  }
  write_cv_pixel_buffer(source.buffer,
                        test_case.y_code,
                        test_case.u_code,
                        test_case.v_code,
                        p010);
  VPMacOSNativeCVPixelBufferPresentFrame cv_frame = {};
  cv_frame.pixel_buffer = source.buffer;
  cv_frame.pixel_format = p010 ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
                               : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  cv_frame.plane_count = 2;
  cv_frame.is_p010 = p010 ? 1 : 0;
  cv_frame.coded_width = kWidth;
  cv_frame.coded_height = kHeight;
  cv_frame.decision = make_decision(test_case.yuv_format, test_case.config);
  VPMacOSNativeFrameInfo cv_info = {};
  char cv_error[512] = {};
  const int cv_ret = VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayout(
      uploader,
      &cv_frame,
      cv_target.buffer,
      kWidth,
      kHeight,
      &cv_info,
      cv_error,
      sizeof(cv_error));
  if (cv_ret != 0) {
    std::fprintf(stderr, "%s cvpixelbuffer failed: Metal upload error=%s\n",
                 test_case.name,
                 cv_error);
    return false;
  }
  Rgba cv_actual;
  if (!read_rgba_half(cv_target.buffer, 2, 2, &cv_actual)) {
    std::fprintf(stderr, "%s cvpixelbuffer failed: could not read RGBA half target\n",
                 test_case.name);
    return false;
  }
  const double cv_dg = std::abs(cv_actual.g - cv_expected.g);
  const double cv_db = std::abs(cv_actual.b - cv_expected.b);
  const double cv_da = std::abs(cv_actual.a - 1.0);
  const double cv_dr_expected = std::abs(cv_actual.r - cv_expected.r);
  if (cv_dr_expected > test_case.tolerance || cv_dg > test_case.tolerance ||
      cv_db > test_case.tolerance || cv_da > test_case.tolerance) {
    std::fprintf(stderr,
                 "%s cvpixelbuffer failed: actual=(%.6f, %.6f, %.6f, %.6f) "
                 "expected=(%.6f, %.6f, %.6f, 1.000000) "
                 "diff=(%.6f, %.6f, %.6f, %.6f) tolerance=%.6f\n",
                 test_case.name,
                 cv_actual.r,
                 cv_actual.g,
                 cv_actual.b,
                 cv_actual.a,
                 cv_expected.r,
                 cv_expected.g,
                 cv_expected.b,
                 cv_dr_expected,
                 cv_dg,
                 cv_db,
                 cv_da,
                 test_case.tolerance);
    return false;
  }
  return true;
}

vr::ColorReferenceConfig config(int matrix, int transfer, int primaries) {
  return {
      vr::VIDEO_COLOR_RANGE_LIMITED,
      matrix,
      transfer,
      primaries,
      true,
  };
}

} // namespace

int main() {
  VPMacOSMetalUploader* uploader = VPMacOSMetalUploaderCreate();
  if (!uploader) {
    return fail("failed to create native Metal uploader");
  }
  if (VPMacOSMetalUploaderIsAvailable(uploader) == 0) {
    VPMacOSMetalUploaderDestroy(uploader);
    return fail("native Metal uploader is not available");
  }

  const int pq_y_203 = static_cast<int>(limited_code_from_luma(0.5806889, 10));
  const int p010_neutral = static_cast<int>(neutral_chroma_code(10));
  const std::vector<Case> cases = {
      {
          "metal cpu-ref parity sdr black",
          16,
          128,
          128,
          8,
          VPMacOSNativePresentFormatNV12,
          config(vr::VIDEO_COLOR_MATRIX_BT709,
                 vr::VIDEO_COLOR_TRANSFER_SDR,
                 vr::VIDEO_COLOR_PRIMARIES_BT709),
          0.002,
      },
      {
          "metal cpu-ref parity sdr white",
          235,
          128,
          128,
          8,
          VPMacOSNativePresentFormatNV12,
          config(vr::VIDEO_COLOR_MATRIX_BT709,
                 vr::VIDEO_COLOR_TRANSFER_SDR,
                 vr::VIDEO_COLOR_PRIMARIES_BT709),
          0.002,
      },
      {
          "metal cpu-ref parity hlg bt2020 white",
          235,
          128,
          128,
          8,
          VPMacOSNativePresentFormatNV12,
          config(vr::VIDEO_COLOR_MATRIX_BT2020_NCL,
                 vr::VIDEO_COLOR_TRANSFER_HLG,
                 vr::VIDEO_COLOR_PRIMARIES_BT2020),
          0.012,
      },
      {
          "metal cpu-ref parity pq p010 203 nits",
          pq_y_203,
          p010_neutral,
          p010_neutral,
          10,
          VPMacOSNativePresentFormatP010,
          config(vr::VIDEO_COLOR_MATRIX_BT2020_NCL,
                 vr::VIDEO_COLOR_TRANSFER_PQ,
                 vr::VIDEO_COLOR_PRIMARIES_BT2020),
          0.012,
      },
  };

  bool ok = true;
  for (const auto& test_case : cases) {
    ok = compare_case(uploader, test_case) && ok;
  }

  VPMacOSMetalUploaderDestroy(uploader);
  return ok ? 0 : 1;
}
