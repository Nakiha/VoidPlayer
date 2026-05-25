#include "native_player_bridge.h"

#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {

constexpr const char* kLayoutBgraKernelSource =
#include "macos/metal_pixel_buffer_uploader_shaders.inc"
    ;

struct MetalLayoutParams {
  uint32_t width;
  uint32_t height;
  int32_t mode;
  int32_t track_count;
  float split_pos;
  uint32_t frame_present0;
  uint32_t frame_present1;
  uint32_t frame_present2;
  uint32_t frame_present3;
  int32_t source_width0;
  int32_t source_width1;
  int32_t source_width2;
  int32_t source_width3;
  int32_t source_height0;
  int32_t source_height1;
  int32_t source_height2;
  int32_t source_height3;
  int32_t yuv_format0;
  int32_t yuv_format1;
  int32_t yuv_format2;
  int32_t yuv_format3;
  uint32_t y_offset0;
  uint32_t y_offset1;
  uint32_t y_offset2;
  uint32_t y_offset3;
  uint32_t uv_offset0;
  uint32_t uv_offset1;
  uint32_t uv_offset2;
  uint32_t uv_offset3;
  uint32_t v_offset0;
  uint32_t v_offset1;
  uint32_t v_offset2;
  uint32_t v_offset3;
  uint32_t y_stride0;
  uint32_t y_stride1;
  uint32_t y_stride2;
  uint32_t y_stride3;
  uint32_t uv_stride0;
  uint32_t uv_stride1;
  uint32_t uv_stride2;
  uint32_t uv_stride3;
  int32_t coded_width0;
  int32_t coded_width1;
  int32_t coded_width2;
  int32_t coded_width3;
  int32_t coded_height0;
  int32_t coded_height1;
  int32_t coded_height2;
  int32_t coded_height3;
  float nv12_uv_scale_x0;
  float nv12_uv_scale_x1;
  float nv12_uv_scale_x2;
  float nv12_uv_scale_x3;
  float nv12_uv_scale_y0;
  float nv12_uv_scale_y1;
  float nv12_uv_scale_y2;
  float nv12_uv_scale_y3;
  int32_t color_range0;
  int32_t color_range1;
  int32_t color_range2;
  int32_t color_range3;
  int32_t color_matrix0;
  int32_t color_matrix1;
  int32_t color_matrix2;
  int32_t color_matrix3;
  int32_t color_transfer0;
  int32_t color_transfer1;
  int32_t color_transfer2;
  int32_t color_transfer3;
  int32_t color_primaries0;
  int32_t color_primaries1;
  int32_t color_primaries2;
  int32_t color_primaries3;
  int32_t order0;
  int32_t order1;
  int32_t order2;
  int32_t order3;
  float display_offset_x0;
  float display_offset_x1;
  float display_offset_x2;
  float display_offset_x3;
  float display_offset_y0;
  float display_offset_y1;
  float display_offset_y2;
  float display_offset_y3;
  float inv_display_size_x0;
  float inv_display_size_x1;
  float inv_display_size_x2;
  float inv_display_size_x3;
  float inv_display_size_y0;
  float inv_display_size_y1;
  float inv_display_size_y2;
  float inv_display_size_y3;
  float view_offset_uv_x0;
  float view_offset_uv_x1;
  float view_offset_uv_x2;
  float view_offset_uv_x3;
  float view_offset_uv_y0;
  float view_offset_uv_y1;
  float view_offset_uv_y2;
  float view_offset_uv_y3;
};

void write_error(char* error, size_t error_size, const char* message) {
  if (!error || error_size == 0) {
    return;
  }
  const size_t length = message ? std::strlen(message) : 0;
  const size_t copy_size = std::min(error_size - 1, length);
  if (copy_size > 0) {
    std::memcpy(error, message, copy_size);
  }
  error[copy_size] = '\0';
}

bool checked_mul_size(size_t lhs, size_t rhs, size_t* out) {
  if (!out) {
    return false;
  }
  if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
    return false;
  }
  *out = lhs * rhs;
  return true;
}

int metal_upload_failure(char* error, size_t error_size, const char* message) {
  write_error(error, error_size, message);
  return -2;
}

int64_t elapsed_us_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

const char* metal_uploader_status_message(int status) {
  switch (status) {
  case VPMacOSMetalUploaderStatusOk:
    return "";
  case VPMacOSMetalUploaderStatusUnavailable:
    return "native Metal uploader is not available";
  case VPMacOSMetalUploaderStatusInvalidArguments:
    return "invalid native Metal pixel buffer validation arguments";
  case VPMacOSMetalUploaderStatusSizeMismatch:
    return "native Metal pixel buffer dimensions do not match the presentation surface";
  case VPMacOSMetalUploaderStatusUnsupportedPixelFormat:
    return "native Metal pixel buffer must be 32-bit BGRA";
  case VPMacOSMetalUploaderStatusTextureWrapFailed:
    return "failed to wrap CVPixelBuffer as a Metal BGRA texture";
  default:
    return "unknown native Metal pixel buffer validation failure";
  }
}

void write_first_present_frame_info(const VPMacOSNativePresentDecisionInfo& decisionInfo,
                                    VPMacOSNativeFrameInfo* out) {
  if (!out) {
    return;
  }
  *out = {};
  for (int slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    if (decisionInfo.frames[slot].present) {
      out->width = decisionInfo.frames[slot].width;
      out->height = decisionInfo.frames[slot].height;
      out->pts_us = decisionInfo.frames[slot].pts_us;
      out->dts_us = decisionInfo.frames[slot].dts_us;
      out->duration_us = decisionInfo.frames[slot].duration_us;
      break;
    }
  }
}

void fill_metal_layout_params(MetalLayoutParams& metalParams,
                              const VPMacOSNativePresentDecisionInfo& decisionInfo,
                              int32_t width,
                              int32_t height) {
  metalParams.width = static_cast<uint32_t>(width);
  metalParams.height = static_cast<uint32_t>(height);
  metalParams.mode = decisionInfo.mode;
  metalParams.track_count = decisionInfo.track_count;
  metalParams.split_pos = decisionInfo.split_pos;
  metalParams.frame_present0 =
      static_cast<uint32_t>(decisionInfo.frames[0].present ? 1u : 0u);
  metalParams.frame_present1 =
      static_cast<uint32_t>(decisionInfo.frames[1].present ? 1u : 0u);
  metalParams.frame_present2 =
      static_cast<uint32_t>(decisionInfo.frames[2].present ? 1u : 0u);
  metalParams.frame_present3 =
      static_cast<uint32_t>(decisionInfo.frames[3].present ? 1u : 0u);
  metalParams.source_width0 = decisionInfo.source_width[0];
  metalParams.source_width1 = decisionInfo.source_width[1];
  metalParams.source_width2 = decisionInfo.source_width[2];
  metalParams.source_width3 = decisionInfo.source_width[3];
  metalParams.source_height0 = decisionInfo.source_height[0];
  metalParams.source_height1 = decisionInfo.source_height[1];
  metalParams.source_height2 = decisionInfo.source_height[2];
  metalParams.source_height3 = decisionInfo.source_height[3];
  metalParams.yuv_format0 = decisionInfo.yuv_format[0];
  metalParams.yuv_format1 = decisionInfo.yuv_format[1];
  metalParams.yuv_format2 = decisionInfo.yuv_format[2];
  metalParams.yuv_format3 = decisionInfo.yuv_format[3];
  metalParams.y_offset0 = static_cast<uint32_t>(std::max(0, decisionInfo.y_offset[0]));
  metalParams.y_offset1 = static_cast<uint32_t>(std::max(0, decisionInfo.y_offset[1]));
  metalParams.y_offset2 = static_cast<uint32_t>(std::max(0, decisionInfo.y_offset[2]));
  metalParams.y_offset3 = static_cast<uint32_t>(std::max(0, decisionInfo.y_offset[3]));
  metalParams.uv_offset0 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_offset[0]));
  metalParams.uv_offset1 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_offset[1]));
  metalParams.uv_offset2 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_offset[2]));
  metalParams.uv_offset3 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_offset[3]));
  metalParams.v_offset0 = static_cast<uint32_t>(std::max(0, decisionInfo.v_offset[0]));
  metalParams.v_offset1 = static_cast<uint32_t>(std::max(0, decisionInfo.v_offset[1]));
  metalParams.v_offset2 = static_cast<uint32_t>(std::max(0, decisionInfo.v_offset[2]));
  metalParams.v_offset3 = static_cast<uint32_t>(std::max(0, decisionInfo.v_offset[3]));
  metalParams.y_stride0 = static_cast<uint32_t>(std::max(0, decisionInfo.y_stride[0]));
  metalParams.y_stride1 = static_cast<uint32_t>(std::max(0, decisionInfo.y_stride[1]));
  metalParams.y_stride2 = static_cast<uint32_t>(std::max(0, decisionInfo.y_stride[2]));
  metalParams.y_stride3 = static_cast<uint32_t>(std::max(0, decisionInfo.y_stride[3]));
  metalParams.uv_stride0 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_stride[0]));
  metalParams.uv_stride1 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_stride[1]));
  metalParams.uv_stride2 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_stride[2]));
  metalParams.uv_stride3 = static_cast<uint32_t>(std::max(0, decisionInfo.uv_stride[3]));
  metalParams.coded_width0 = decisionInfo.coded_width[0];
  metalParams.coded_width1 = decisionInfo.coded_width[1];
  metalParams.coded_width2 = decisionInfo.coded_width[2];
  metalParams.coded_width3 = decisionInfo.coded_width[3];
  metalParams.coded_height0 = decisionInfo.coded_height[0];
  metalParams.coded_height1 = decisionInfo.coded_height[1];
  metalParams.coded_height2 = decisionInfo.coded_height[2];
  metalParams.coded_height3 = decisionInfo.coded_height[3];
  metalParams.nv12_uv_scale_x0 = decisionInfo.nv12_uv_scale_x[0];
  metalParams.nv12_uv_scale_x1 = decisionInfo.nv12_uv_scale_x[1];
  metalParams.nv12_uv_scale_x2 = decisionInfo.nv12_uv_scale_x[2];
  metalParams.nv12_uv_scale_x3 = decisionInfo.nv12_uv_scale_x[3];
  metalParams.nv12_uv_scale_y0 = decisionInfo.nv12_uv_scale_y[0];
  metalParams.nv12_uv_scale_y1 = decisionInfo.nv12_uv_scale_y[1];
  metalParams.nv12_uv_scale_y2 = decisionInfo.nv12_uv_scale_y[2];
  metalParams.nv12_uv_scale_y3 = decisionInfo.nv12_uv_scale_y[3];
  metalParams.color_range0 = decisionInfo.color_range[0];
  metalParams.color_range1 = decisionInfo.color_range[1];
  metalParams.color_range2 = decisionInfo.color_range[2];
  metalParams.color_range3 = decisionInfo.color_range[3];
  metalParams.color_matrix0 = decisionInfo.color_matrix[0];
  metalParams.color_matrix1 = decisionInfo.color_matrix[1];
  metalParams.color_matrix2 = decisionInfo.color_matrix[2];
  metalParams.color_matrix3 = decisionInfo.color_matrix[3];
  metalParams.color_transfer0 = decisionInfo.color_transfer[0];
  metalParams.color_transfer1 = decisionInfo.color_transfer[1];
  metalParams.color_transfer2 = decisionInfo.color_transfer[2];
  metalParams.color_transfer3 = decisionInfo.color_transfer[3];
  metalParams.color_primaries0 = decisionInfo.color_primaries[0];
  metalParams.color_primaries1 = decisionInfo.color_primaries[1];
  metalParams.color_primaries2 = decisionInfo.color_primaries[2];
  metalParams.color_primaries3 = decisionInfo.color_primaries[3];
  metalParams.order0 = decisionInfo.order[0];
  metalParams.order1 = decisionInfo.order[1];
  metalParams.order2 = decisionInfo.order[2];
  metalParams.order3 = decisionInfo.order[3];
  metalParams.display_offset_x0 = decisionInfo.display_offset_x[0];
  metalParams.display_offset_x1 = decisionInfo.display_offset_x[1];
  metalParams.display_offset_x2 = decisionInfo.display_offset_x[2];
  metalParams.display_offset_x3 = decisionInfo.display_offset_x[3];
  metalParams.display_offset_y0 = decisionInfo.display_offset_y[0];
  metalParams.display_offset_y1 = decisionInfo.display_offset_y[1];
  metalParams.display_offset_y2 = decisionInfo.display_offset_y[2];
  metalParams.display_offset_y3 = decisionInfo.display_offset_y[3];
  metalParams.inv_display_size_x0 = decisionInfo.inv_display_size_x[0];
  metalParams.inv_display_size_x1 = decisionInfo.inv_display_size_x[1];
  metalParams.inv_display_size_x2 = decisionInfo.inv_display_size_x[2];
  metalParams.inv_display_size_x3 = decisionInfo.inv_display_size_x[3];
  metalParams.inv_display_size_y0 = decisionInfo.inv_display_size_y[0];
  metalParams.inv_display_size_y1 = decisionInfo.inv_display_size_y[1];
  metalParams.inv_display_size_y2 = decisionInfo.inv_display_size_y[2];
  metalParams.inv_display_size_y3 = decisionInfo.inv_display_size_y[3];
  metalParams.view_offset_uv_x0 = decisionInfo.view_offset_uv_x[0];
  metalParams.view_offset_uv_x1 = decisionInfo.view_offset_uv_x[1];
  metalParams.view_offset_uv_x2 = decisionInfo.view_offset_uv_x[2];
  metalParams.view_offset_uv_x3 = decisionInfo.view_offset_uv_x[3];
  metalParams.view_offset_uv_y0 = decisionInfo.view_offset_uv_y[0];
  metalParams.view_offset_uv_y1 = decisionInfo.view_offset_uv_y[1];
  metalParams.view_offset_uv_y2 = decisionInfo.view_offset_uv_y[2];
  metalParams.view_offset_uv_y3 = decisionInfo.view_offset_uv_y[3];
}

}  // namespace

@interface VPMacOSMetalUploaderImpl : NSObject {
 @private
  id<MTLDevice> _device;
  id<MTLCommandQueue> _commandQueue;
  id<MTLBuffer> _stagingBuffer;
  id<MTLBuffer> _layoutParamsBuffer;
  id<MTLComputePipelineState> _layoutPipeline;
  id<MTLComputePipelineState> _cvPixelBufferPipeline;
  CVMetalTextureCacheRef _textureCache;
  std::atomic<int64_t> _directYuvUploadCount;
  std::atomic<int64_t> _cvPixelBufferUploadCount;
  std::atomic<int64_t> _presentPackageUploadCount;
  std::atomic<int64_t> _lastPresentPackageCopyUs;
  std::atomic<int64_t> _lastPresentPackageGpuWaitUs;
  std::atomic<int64_t> _lastPresentPackageTotalUs;
  std::atomic<int32_t> _lastPresentPackageStorage;
}

- (BOOL)isAvailable;
- (int64_t)directYuvUploadCount;
- (int64_t)cvPixelBufferUploadCount;
- (int64_t)presentPackageUploadCount;
- (int64_t)lastPresentPackageCopyUs;
- (int64_t)lastPresentPackageGpuWaitUs;
- (int64_t)lastPresentPackageTotalUs;
- (int32_t)lastPresentPackageStorage;
- (int)validatePixelBufferStatus:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height;
- (BOOL)validatePixelBuffer:(CVPixelBufferRef)pixelBuffer
                      width:(int32_t)width
                     height:(int32_t)height;
- (int)copyPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           data:(const uint8_t*)data
                       dataSize:(size_t)dataSize
                  toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                          width:(int32_t)width
                         height:(int32_t)height
                            out:(VPMacOSNativeFrameInfo*)out
                          error:(char*)error
                      errorSize:(size_t)errorSize;
- (int)copyCVPixelBufferPresentFrame:(const VPMacOSNativeCVPixelBufferPresentFrame*)frame
                        toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                width:(int32_t)width
                               height:(int32_t)height
                                  out:(VPMacOSNativeFrameInfo*)out
                                error:(char*)error
                            errorSize:(size_t)errorSize;
- (int)uploadPreparedPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                   width:(int32_t)width
                                  height:(int32_t)height
                                     out:(VPMacOSNativeFrameInfo*)out
                                   error:(char*)error
                               errorSize:(size_t)errorSize;

@end

@implementation VPMacOSMetalUploaderImpl

- (instancetype)init {
  self = [super init];
  if (self) {
    _directYuvUploadCount.store(0, std::memory_order_relaxed);
    _cvPixelBufferUploadCount.store(0, std::memory_order_relaxed);
    _presentPackageUploadCount.store(0, std::memory_order_relaxed);
    _lastPresentPackageCopyUs.store(0, std::memory_order_relaxed);
    _lastPresentPackageGpuWaitUs.store(0, std::memory_order_relaxed);
    _lastPresentPackageTotalUs.store(0, std::memory_order_relaxed);
    _lastPresentPackageStorage.store(
        VPMacOSNativePresentPackageStorageUnavailable,
        std::memory_order_relaxed);
    _device = MTLCreateSystemDefaultDevice();
    if (_device) {
      _commandQueue = [_device newCommandQueue];
      CVMetalTextureCacheRef cache = nullptr;
      if (CVMetalTextureCacheCreate(
              kCFAllocatorDefault, nullptr, _device, nullptr, &cache) ==
          kCVReturnSuccess) {
        _textureCache = cache;
      }
      NSError* libraryError = nil;
      NSString* source =
          [[NSString alloc] initWithUTF8String:kLayoutBgraKernelSource];
      id<MTLLibrary> library = [_device newLibraryWithSource:source
                                                     options:nil
                                                       error:&libraryError];
      id<MTLFunction> function =
          library ? [library newFunctionWithName:@"layout_bgra_copy"] : nil;
      if (function) {
        NSError* pipelineError = nil;
        _layoutPipeline = [_device newComputePipelineStateWithFunction:function
                                                                  error:&pipelineError];
      }
      id<MTLFunction> cvFunction =
          library ? [library newFunctionWithName:@"layout_cv_yuv_copy"] : nil;
      if (cvFunction) {
        NSError* pipelineError = nil;
        _cvPixelBufferPipeline = [_device newComputePipelineStateWithFunction:cvFunction
                                                                        error:&pipelineError];
      }
    }
  }
  return self;
}

- (void)dealloc {
  if (_textureCache) {
    CFRelease(_textureCache);
    _textureCache = nullptr;
  }
}

- (BOOL)isAvailable {
  return _device != nil && _commandQueue != nil && _textureCache != nullptr;
}

- (int64_t)directYuvUploadCount {
  return _directYuvUploadCount.load(std::memory_order_relaxed);
}

- (int64_t)cvPixelBufferUploadCount {
  return _cvPixelBufferUploadCount.load(std::memory_order_relaxed);
}

- (int64_t)presentPackageUploadCount {
  return _presentPackageUploadCount.load(std::memory_order_relaxed);
}

- (int64_t)lastPresentPackageCopyUs {
  return _lastPresentPackageCopyUs.load(std::memory_order_relaxed);
}

- (int64_t)lastPresentPackageGpuWaitUs {
  return _lastPresentPackageGpuWaitUs.load(std::memory_order_relaxed);
}

- (int64_t)lastPresentPackageTotalUs {
  return _lastPresentPackageTotalUs.load(std::memory_order_relaxed);
}

- (int32_t)lastPresentPackageStorage {
  return _lastPresentPackageStorage.load(std::memory_order_relaxed);
}

- (int)validatePixelBufferStatus:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height {
  if (![self isAvailable] || !pixelBuffer || width <= 0 || height <= 0) {
    return [self isAvailable]
        ? VPMacOSMetalUploaderStatusInvalidArguments
        : VPMacOSMetalUploaderStatusUnavailable;
  }
  if (CVPixelBufferGetWidth(pixelBuffer) != static_cast<size_t>(width) ||
      CVPixelBufferGetHeight(pixelBuffer) != static_cast<size_t>(height)) {
    return VPMacOSMetalUploaderStatusSizeMismatch;
  }
  if (CVPixelBufferGetPixelFormatType(pixelBuffer) != kCVPixelFormatType_32BGRA) {
    return VPMacOSMetalUploaderStatusUnsupportedPixelFormat;
  }
  CVMetalTextureRef metalTextureRef = nullptr;
  const CVReturn status = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      _textureCache,
      pixelBuffer,
      nullptr,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &metalTextureRef);
  if (status != kCVReturnSuccess || !metalTextureRef) {
    return VPMacOSMetalUploaderStatusTextureWrapFailed;
  }
  id<MTLTexture> texture = CVMetalTextureGetTexture(metalTextureRef);
  const BOOL valid = texture != nil;
  CFRelease(metalTextureRef);
  return valid
      ? VPMacOSMetalUploaderStatusOk
      : VPMacOSMetalUploaderStatusTextureWrapFailed;
}

- (BOOL)validatePixelBuffer:(CVPixelBufferRef)pixelBuffer
                      width:(int32_t)width
                     height:(int32_t)height {
  return [self validatePixelBufferStatus:pixelBuffer width:width height:height] ==
      VPMacOSMetalUploaderStatusOk;
}

- (BOOL)ensureStagingBufferWithLength:(size_t)length {
  if (_stagingBuffer != nil && [_stagingBuffer length] >= length) {
    return YES;
  }
  _stagingBuffer = [_device newBufferWithLength:length
                                        options:MTLResourceStorageModeShared];
  return _stagingBuffer != nil;
}

- (BOOL)ensureLayoutParamsBuffer {
  if (_layoutParamsBuffer != nil && [_layoutParamsBuffer length] >= sizeof(MetalLayoutParams)) {
    return YES;
  }
  _layoutParamsBuffer = [_device newBufferWithLength:sizeof(MetalLayoutParams)
                                             options:MTLResourceStorageModeShared];
  return _layoutParamsBuffer != nil;
}

- (int)copyPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           data:(const uint8_t*)data
                       dataSize:(size_t)dataSize
                  toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                          width:(int32_t)width
                         height:(int32_t)height
                            out:(VPMacOSNativeFrameInfo*)out
                          error:(char*)error
                      errorSize:(size_t)errorSize {
  if (![self isAvailable] || !_layoutPipeline) {
    write_error(error, errorSize, "native Metal layout uploader is not available");
    return -1;
  }
  if (!package || !data || dataSize == 0 || package->used_bytes == 0 ||
      package->used_bytes > dataSize) {
    write_error(error, errorSize, "invalid native Metal present package arguments");
    return -1;
  }
  if (![self ensureStagingBufferWithLength:package->used_bytes] ||
      ![self ensureLayoutParamsBuffer]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal layout buffers");
  }
  const auto totalStart = std::chrono::steady_clock::now();
  const auto copyStart = std::chrono::steady_clock::now();
  std::memcpy([_stagingBuffer contents], data, package->used_bytes);
  _lastPresentPackageCopyUs.store(elapsed_us_since(copyStart), std::memory_order_relaxed);
  const int uploadRet = [self uploadPreparedPresentFramePackage:package
                                                  toPixelBuffer:pixelBuffer
                                                          width:width
                                                         height:height
                                                            out:out
                                                          error:error
                                                      errorSize:errorSize];
  _lastPresentPackageTotalUs.store(elapsed_us_since(totalStart), std::memory_order_relaxed);
  return uploadRet;
}

- (int)uploadPreparedPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                   width:(int32_t)width
                                  height:(int32_t)height
                                     out:(VPMacOSNativeFrameInfo*)out
                                   error:(char*)error
                               errorSize:(size_t)errorSize {
  if (![self isAvailable] || !_layoutPipeline) {
    write_error(error, errorSize, "native Metal layout uploader is not available");
    return -1;
  }
  if (!package || !pixelBuffer || width <= 0 || height <= 0 ||
      package->storage == VPMacOSNativePresentPackageStorageUnavailable) {
    write_error(error, errorSize, "invalid native Metal present package arguments");
    return -1;
  }
  const int validationStatus =
      [self validatePixelBufferStatus:pixelBuffer width:width height:height];
  if (validationStatus != VPMacOSMetalUploaderStatusOk) {
    write_error(error, errorSize, metal_uploader_status_message(validationStatus));
    return -1;
  }

  const auto& decisionInfo = package->decision;
  if (package->storage == VPMacOSNativePresentPackageStorageYUV) {
    _directYuvUploadCount.fetch_add(1, std::memory_order_relaxed);
  }
  write_first_present_frame_info(decisionInfo, out);

  auto* metalParams = static_cast<MetalLayoutParams*>([_layoutParamsBuffer contents]);
  fill_metal_layout_params(*metalParams, decisionInfo, width, height);

  CVMetalTextureRef metalTextureRef = nullptr;
  const CVReturn textureStatus = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      _textureCache,
      pixelBuffer,
      nullptr,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &metalTextureRef);
  if (textureStatus != kCVReturnSuccess || !metalTextureRef) {
    return metal_upload_failure(
        error, errorSize, "failed to wrap CVPixelBuffer as a Metal texture");
  }

  id<MTLTexture> destinationTexture = CVMetalTextureGetTexture(metalTextureRef);
  const auto gpuStart = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
  if (!destinationTexture || !commandBuffer || !compute) {
    CFRelease(metalTextureRef);
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal layout compute command");
  }

  [compute setComputePipelineState:_layoutPipeline];
  [compute setBuffer:_stagingBuffer offset:0 atIndex:0];
  [compute setBuffer:_layoutParamsBuffer offset:0 atIndex:1];
  [compute setTexture:destinationTexture atIndex:0];

  const NSUInteger threadWidth = _layoutPipeline.threadExecutionWidth;
  const NSUInteger threadHeight =
      std::max<NSUInteger>(1, _layoutPipeline.maxTotalThreadsPerThreadgroup / threadWidth);
  const MTLSize threadsPerThreadgroup = MTLSizeMake(threadWidth, threadHeight, 1);
  const MTLSize threads = MTLSizeMake(width, height, 1);
  [compute dispatchThreads:threads threadsPerThreadgroup:threadsPerThreadgroup];
  [compute endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  const BOOL completed = [commandBuffer status] == MTLCommandBufferStatusCompleted;
  _lastPresentPackageGpuWaitUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
  CFRelease(metalTextureRef);
  if (!completed) {
    return metal_upload_failure(
        error, errorSize, "native Metal layout compute did not complete");
  }

  _presentPackageUploadCount.fetch_add(1, std::memory_order_relaxed);
  _lastPresentPackageStorage.store(package->storage, std::memory_order_relaxed);
  write_error(error, errorSize, "");
  return 0;
}

- (int)copyCVPixelBufferPresentFrame:(const VPMacOSNativeCVPixelBufferPresentFrame*)frame
                        toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                width:(int32_t)width
                               height:(int32_t)height
                                  out:(VPMacOSNativeFrameInfo*)out
                                error:(char*)error
                            errorSize:(size_t)errorSize {
  if (![self isAvailable] || !_cvPixelBufferPipeline) {
    write_error(error, errorSize, "native Metal CVPixelBuffer uploader is not available");
    return -1;
  }
  if (!frame || !frame->pixel_buffer || !pixelBuffer || !out ||
      width <= 0 || height <= 0 || frame->plane_count < 2 ||
      frame->coded_width <= 0 || frame->coded_height <= 0) {
    write_error(error, errorSize, "invalid native Metal CVPixelBuffer upload arguments");
    return -1;
  }
  const int validationStatus =
      [self validatePixelBufferStatus:pixelBuffer width:width height:height];
  if (validationStatus != VPMacOSMetalUploaderStatusOk) {
    write_error(error, errorSize, metal_uploader_status_message(validationStatus));
    return -1;
  }
  if (![self ensureLayoutParamsBuffer]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal layout buffers");
  }

  auto* metalParams = static_cast<MetalLayoutParams*>([_layoutParamsBuffer contents]);
  fill_metal_layout_params(*metalParams, frame->decision, width, height);
  write_first_present_frame_info(frame->decision, out);

  CVPixelBufferRef sourcePixelBuffer =
      static_cast<CVPixelBufferRef>(frame->pixel_buffer);
  const bool isP010 = frame->is_p010 != 0;
  const MTLPixelFormat yFormat = isP010 ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
  const MTLPixelFormat uvFormat = isP010 ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;
  CVMetalTextureRef sourceYRef = nullptr;
  CVMetalTextureRef sourceUVRef = nullptr;
  CVMetalTextureRef destinationRef = nullptr;
  const CVReturn yStatus = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      _textureCache,
      sourcePixelBuffer,
      nullptr,
      yFormat,
      CVPixelBufferGetWidthOfPlane(sourcePixelBuffer, 0),
      CVPixelBufferGetHeightOfPlane(sourcePixelBuffer, 0),
      0,
      &sourceYRef);
  const CVReturn uvStatus = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      _textureCache,
      sourcePixelBuffer,
      nullptr,
      uvFormat,
      CVPixelBufferGetWidthOfPlane(sourcePixelBuffer, 1),
      CVPixelBufferGetHeightOfPlane(sourcePixelBuffer, 1),
      1,
      &sourceUVRef);
  const CVReturn destinationStatus = CVMetalTextureCacheCreateTextureFromImage(
      kCFAllocatorDefault,
      _textureCache,
      pixelBuffer,
      nullptr,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &destinationRef);
  if (yStatus != kCVReturnSuccess || uvStatus != kCVReturnSuccess ||
      destinationStatus != kCVReturnSuccess || !sourceYRef || !sourceUVRef ||
      !destinationRef) {
    if (sourceYRef) CFRelease(sourceYRef);
    if (sourceUVRef) CFRelease(sourceUVRef);
    if (destinationRef) CFRelease(destinationRef);
    return metal_upload_failure(
        error, errorSize, "failed to wrap CVPixelBuffer planes as Metal textures");
  }

  id<MTLTexture> sourceYTexture = CVMetalTextureGetTexture(sourceYRef);
  id<MTLTexture> sourceUVTexture = CVMetalTextureGetTexture(sourceUVRef);
  id<MTLTexture> destinationTexture = CVMetalTextureGetTexture(destinationRef);
  const auto gpuStart = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
  if (!sourceYTexture || !sourceUVTexture || !destinationTexture ||
      !commandBuffer || !compute) {
    CFRelease(sourceYRef);
    CFRelease(sourceUVRef);
    CFRelease(destinationRef);
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal CVPixelBuffer compute command");
  }

  [compute setComputePipelineState:_cvPixelBufferPipeline];
  [compute setBuffer:_layoutParamsBuffer offset:0 atIndex:0];
  [compute setTexture:destinationTexture atIndex:0];
  [compute setTexture:sourceYTexture atIndex:1];
  [compute setTexture:sourceUVTexture atIndex:2];

  const NSUInteger threadWidth = _cvPixelBufferPipeline.threadExecutionWidth;
  const NSUInteger threadHeight =
      std::max<NSUInteger>(1, _cvPixelBufferPipeline.maxTotalThreadsPerThreadgroup / threadWidth);
  const MTLSize threadsPerThreadgroup = MTLSizeMake(threadWidth, threadHeight, 1);
  const MTLSize threads = MTLSizeMake(width, height, 1);
  [compute dispatchThreads:threads threadsPerThreadgroup:threadsPerThreadgroup];
  [compute endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  const BOOL completed = [commandBuffer status] == MTLCommandBufferStatusCompleted;
  _lastPresentPackageGpuWaitUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
  CFRelease(sourceYRef);
  CFRelease(sourceUVRef);
  CFRelease(destinationRef);
  if (!completed) {
    return metal_upload_failure(
        error, errorSize, "native Metal CVPixelBuffer compute did not complete");
  }

  _cvPixelBufferUploadCount.fetch_add(1, std::memory_order_relaxed);
  _lastPresentPackageCopyUs.store(0, std::memory_order_relaxed);
  _lastPresentPackageTotalUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
  _lastPresentPackageStorage.store(VPMacOSNativePresentPackageStorageCVPixelBuffer,
                                   std::memory_order_relaxed);
  write_error(error, errorSize, "");
  return 0;
}

@end

struct VPMacOSMetalUploader {
  VPMacOSMetalUploaderImpl* impl;
};

VPMacOSMetalUploader* VPMacOSMetalUploaderCreate(void) {
  VPMacOSMetalUploaderImpl* impl = [[VPMacOSMetalUploaderImpl alloc] init];
  if (!impl) {
    return nullptr;
  }
  auto* uploader = new VPMacOSMetalUploader{impl};
  return uploader;
}

void VPMacOSMetalUploaderDestroy(VPMacOSMetalUploader* uploader) {
  delete uploader;
}

int VPMacOSMetalUploaderIsAvailable(VPMacOSMetalUploader* uploader) {
  return uploader && uploader->impl && [uploader->impl isAvailable] ? 1 : 0;
}

int64_t VPMacOSMetalUploaderDirectYUVUploadCount(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl directYuvUploadCount];
}

int64_t VPMacOSMetalUploaderCVPixelBufferUploadCount(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl cvPixelBufferUploadCount];
}

int64_t VPMacOSMetalUploaderPresentPackageUploadCount(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl presentPackageUploadCount];
}

int64_t VPMacOSMetalUploaderLastPresentPackageCopyUs(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl lastPresentPackageCopyUs];
}

int64_t VPMacOSMetalUploaderLastPresentPackageGpuWaitUs(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl lastPresentPackageGpuWaitUs];
}

int64_t VPMacOSMetalUploaderLastPresentPackageTotalUs(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl lastPresentPackageTotalUs];
}

int32_t VPMacOSMetalUploaderLastPresentPackageStorage(VPMacOSMetalUploader* uploader) {
  if (!uploader || !uploader->impl) {
    return VPMacOSNativePresentPackageStorageUnavailable;
  }
  return [uploader->impl lastPresentPackageStorage];
}

int VPMacOSMetalUploaderValidatePixelBuffer(VPMacOSMetalUploader* uploader,
                                            void* pixel_buffer,
                                            int32_t width,
                                            int32_t height) {
  return VPMacOSMetalUploaderValidatePixelBufferChecked(
      uploader, pixel_buffer, width, height, nullptr, 0) ==
      VPMacOSMetalUploaderStatusOk ? 1 : 0;
}

const char* VPMacOSMetalUploaderStatusMessage(int status) {
  return metal_uploader_status_message(status);
}

int VPMacOSMetalUploaderValidatePixelBufferChecked(VPMacOSMetalUploader* uploader,
                                                   void* pixel_buffer,
                                                   int32_t width,
                                                   int32_t height,
                                                   char* error,
                                                   size_t error_size) {
  int status = VPMacOSMetalUploaderStatusUnavailable;
  if (uploader && uploader->impl) {
    status = [uploader->impl validatePixelBufferStatus:(CVPixelBufferRef)pixel_buffer
                                                width:width
                                               height:height];
  }
  write_error(error, error_size, metal_uploader_status_message(status));
  return status;
}

int VPMacOSMetalUploaderCopyPresentFramePackageWithLayout(
    VPMacOSMetalUploader* uploader,
    const uint8_t* data,
    size_t data_size,
    const VPMacOSNativePresentFramePackageInfo* package,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  if (!uploader || !uploader->impl) {
    write_error(error, error_size, "native Metal uploader is null");
    return -1;
  }
  return [uploader->impl copyPresentFramePackage:package
                                            data:data
                                        dataSize:data_size
                                   toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                           width:width
                                          height:height
                                             out:out
                                           error:error
                                       errorSize:error_size];
}

int VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayout(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeCVPixelBufferPresentFrame* frame,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  if (!uploader || !uploader->impl) {
    write_error(error, error_size, "native Metal uploader is null");
    return -1;
  }
  return [uploader->impl copyCVPixelBufferPresentFrame:frame
                                         toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                                 width:width
                                                height:height
                                                   out:out
                                                 error:error
                                             errorSize:error_size];
}
