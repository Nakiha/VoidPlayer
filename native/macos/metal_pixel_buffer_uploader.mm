#include "macos/metal_uploader_internal.h"

#include "macos/metal_layout_params.h"
#include "macos/metal_texture_wrapping.h"

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {

constexpr const char* kLayoutBgraKernelSource =
#include "macos/metal_pixel_buffer_uploader_shaders.inc"
    ;

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

bool overlay_primitive_set_has_content(
    const VPMacOSNativeOverlayGpuPrimitiveSet* overlay) {
  return overlay && (overlay->fill_rect_count > 0 ||
                     overlay->line_rect_count > 0 ||
                     overlay->motion_line_count > 0);
}

}  // namespace

const char* VPMacOSMetalUploaderStatusMessageForCode(int status) {
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
      id<MTLFunction> cvSetFunction =
          library ? [library newFunctionWithName:@"layout_cv_yuv_set_copy"] : nil;
      if (cvSetFunction) {
        NSError* pipelineError = nil;
        _cvPixelBufferSetPipeline =
            [_device newComputePipelineStateWithFunction:cvSetFunction
                                                   error:&pipelineError];
      }
      id<MTLFunction> overlayFillRectFunction =
          library ? [library newFunctionWithName:@"composite_overlay_fill_rects"] : nil;
      if (overlayFillRectFunction) {
        NSError* pipelineError = nil;
        _overlayFillRectPipeline =
            [_device newComputePipelineStateWithFunction:overlayFillRectFunction
                                                   error:&pipelineError];
      }
      id<MTLFunction> overlayLineMaskFunction =
          library ? [library newFunctionWithName:@"build_overlay_line_mask"] : nil;
      if (overlayLineMaskFunction) {
        NSError* pipelineError = nil;
        _overlayLineMaskPipeline =
            [_device newComputePipelineStateWithFunction:overlayLineMaskFunction
                                                   error:&pipelineError];
      }
      id<MTLFunction> overlayLineContrastFunction =
          library ? [library newFunctionWithName:@"composite_overlay_line_contrast"] : nil;
      if (overlayLineContrastFunction) {
        NSError* pipelineError = nil;
        _overlayLineContrastPipeline =
            [_device newComputePipelineStateWithFunction:overlayLineContrastFunction
                                                   error:&pipelineError];
      }
      id<MTLFunction> overlayMotionLineFunction =
          library ? [library newFunctionWithName:@"composite_overlay_motion_lines"] : nil;
      if (overlayMotionLineFunction) {
        NSError* pipelineError = nil;
        _overlayMotionLinePipeline =
            [_device newComputePipelineStateWithFunction:overlayMotionLineFunction
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
  vp_macos::ScopedCVMetalTexture metalTexture;
  const CVReturn status = vp_macos::create_cv_metal_texture(
      _textureCache,
      pixelBuffer,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &metalTexture);
  if (status != kCVReturnSuccess || !metalTexture.valid()) {
    return VPMacOSMetalUploaderStatusTextureWrapFailed;
  }
  return metalTexture.valid()
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
  if (_layoutParamsBuffer != nil && [_layoutParamsBuffer length] >= sizeof(vp_macos::MetalLayoutParams)) {
    return YES;
  }
  _layoutParamsBuffer = [_device newBufferWithLength:sizeof(vp_macos::MetalLayoutParams)
                                             options:MTLResourceStorageModeShared];
  return _layoutParamsBuffer != nil;
}

- (BOOL)ensureOverlayLineRectBufferWithLength:(size_t)length {
  if (_overlayLineRectBuffer != nil && [_overlayLineRectBuffer length] >= length) {
    return YES;
  }
  _overlayLineRectBuffer =
      [_device newBufferWithLength:length options:MTLResourceStorageModeShared];
  return _overlayLineRectBuffer != nil;
}

- (BOOL)ensureOverlayFillRectBufferWithLength:(size_t)length {
  if (_overlayFillRectBuffer != nil && [_overlayFillRectBuffer length] >= length) {
    return YES;
  }
  _overlayFillRectBuffer =
      [_device newBufferWithLength:length options:MTLResourceStorageModeShared];
  return _overlayFillRectBuffer != nil;
}

- (BOOL)ensureOverlayMotionLineBufferWithLength:(size_t)length {
  if (_overlayMotionLineBuffer != nil && [_overlayMotionLineBuffer length] >= length) {
    return YES;
  }
  _overlayMotionLineBuffer =
      [_device newBufferWithLength:length options:MTLResourceStorageModeShared];
  return _overlayMotionLineBuffer != nil;
}

- (BOOL)ensureOverlayLineMaskBufferWithLength:(size_t)length {
  if (_overlayLineMaskBuffer != nil && [_overlayLineMaskBuffer length] >= length) {
    return YES;
  }
  _overlayLineMaskBuffer =
      [_device newBufferWithLength:length options:MTLResourceStorageModeShared];
  return _overlayLineMaskBuffer != nil;
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
  return [self copyPresentFramePackage:package
                                  data:data
                              dataSize:dataSize
                               overlay:nullptr
                         toPixelBuffer:pixelBuffer
                                 width:width
                                height:height
                                   out:out
                                 error:error
                             errorSize:errorSize];
}

- (int)copyPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           data:(const uint8_t*)data
                       dataSize:(size_t)dataSize
                        overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
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
                                                       overlay:overlay
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
  return [self uploadPreparedPresentFramePackage:package
                                         overlay:nullptr
                                   toPixelBuffer:pixelBuffer
                                           width:width
                                          height:height
                                             out:out
                                           error:error
                                       errorSize:errorSize];
}

- (int)uploadPreparedPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                                 overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
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
    write_error(error, errorSize, VPMacOSMetalUploaderStatusMessageForCode(validationStatus));
    return -1;
  }

  const auto& decisionInfo = package->decision;
  if (package->storage == VPMacOSNativePresentPackageStorageYUV) {
    _directYuvUploadCount.fetch_add(1, std::memory_order_relaxed);
  }
  vp_macos::write_first_present_frame_info(decisionInfo, out);

  auto* metalParams = static_cast<vp_macos::MetalLayoutParams*>([_layoutParamsBuffer contents]);
  vp_macos::fill_metal_layout_params(*metalParams, decisionInfo, width, height);

  vp_macos::ScopedCVMetalTexture destinationRef;
  const CVReturn textureStatus = vp_macos::create_cv_metal_texture(
      _textureCache,
      pixelBuffer,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &destinationRef);
  if (textureStatus != kCVReturnSuccess || !destinationRef.valid()) {
    return metal_upload_failure(
        error, errorSize, "failed to wrap CVPixelBuffer as a Metal texture");
  }

  id<MTLTexture> destinationTexture = destinationRef.texture();
  const auto gpuStart = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
  if (!destinationTexture || !commandBuffer || !compute) {
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
  const int overlayRet = [self encodeOverlayGpuPrimitives:overlay
                                                 decision:&package->decision
                                            commandBuffer:commandBuffer
                                       destinationTexture:destinationTexture
                                                    width:width
                                                   height:height
                                                    error:error
                                                errorSize:errorSize];
  if (overlayRet != 0) {
    return overlayRet;
  }
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  const BOOL completed = [commandBuffer status] == MTLCommandBufferStatusCompleted;
  _lastPresentPackageGpuWaitUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
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
  return [self copyCVPixelBufferPresentFrame:frame
                                     overlay:nullptr
                               toPixelBuffer:pixelBuffer
                                       width:width
                                      height:height
                                         out:out
                                       error:error
                                   errorSize:errorSize];
}

- (int)copyCVPixelBufferPresentFrame:(const VPMacOSNativeCVPixelBufferPresentFrame*)frame
                             overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
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
    write_error(error, errorSize, VPMacOSMetalUploaderStatusMessageForCode(validationStatus));
    return -1;
  }
  if (![self ensureLayoutParamsBuffer]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal layout buffers");
  }

  auto* metalParams = static_cast<vp_macos::MetalLayoutParams*>([_layoutParamsBuffer contents]);
  vp_macos::fill_metal_layout_params(*metalParams, frame->decision, width, height);
  vp_macos::write_first_present_frame_info(frame->decision, out);

  CVPixelBufferRef sourcePixelBuffer =
      static_cast<CVPixelBufferRef>(frame->pixel_buffer);
  const bool isP010 = frame->is_p010 != 0;
  const MTLPixelFormat yFormat = isP010 ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
  const MTLPixelFormat uvFormat = isP010 ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;
  vp_macos::ScopedCVMetalTexture sourceYRef;
  vp_macos::ScopedCVMetalTexture sourceUVRef;
  vp_macos::ScopedCVMetalTexture destinationRef;
  const CVReturn yStatus = vp_macos::create_cv_metal_texture(
      _textureCache,
      sourcePixelBuffer,
      yFormat,
      CVPixelBufferGetWidthOfPlane(sourcePixelBuffer, 0),
      CVPixelBufferGetHeightOfPlane(sourcePixelBuffer, 0),
      0,
      &sourceYRef);
  const CVReturn uvStatus = vp_macos::create_cv_metal_texture(
      _textureCache,
      sourcePixelBuffer,
      uvFormat,
      CVPixelBufferGetWidthOfPlane(sourcePixelBuffer, 1),
      CVPixelBufferGetHeightOfPlane(sourcePixelBuffer, 1),
      1,
      &sourceUVRef);
  const CVReturn destinationStatus = vp_macos::create_cv_metal_texture(
      _textureCache,
      pixelBuffer,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &destinationRef);
  if (yStatus != kCVReturnSuccess || uvStatus != kCVReturnSuccess ||
      destinationStatus != kCVReturnSuccess || !sourceYRef.valid() ||
      !sourceUVRef.valid() || !destinationRef.valid()) {
    return metal_upload_failure(
        error, errorSize, "failed to wrap CVPixelBuffer planes as Metal textures");
  }

  id<MTLTexture> sourceYTexture = sourceYRef.texture();
  id<MTLTexture> sourceUVTexture = sourceUVRef.texture();
  id<MTLTexture> destinationTexture = destinationRef.texture();
  const auto gpuStart = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
  if (!sourceYTexture || !sourceUVTexture || !destinationTexture ||
      !commandBuffer || !compute) {
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
  const int overlayRet = [self encodeOverlayGpuPrimitives:overlay
                                                 decision:&frame->decision
                                            commandBuffer:commandBuffer
                                       destinationTexture:destinationTexture
                                                    width:width
                                                   height:height
                                                    error:error
                                                errorSize:errorSize];
  if (overlayRet != 0) {
    return overlayRet;
  }
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  const BOOL completed = [commandBuffer status] == MTLCommandBufferStatusCompleted;
  _lastPresentPackageGpuWaitUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
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

- (int)copyCVPixelBufferPresentFrameSet:(const VPMacOSNativeCVPixelBufferPresentFrameSet*)frameSet
                          toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                  width:(int32_t)width
                                 height:(int32_t)height
                                    out:(VPMacOSNativeFrameInfo*)out
                                  error:(char*)error
                              errorSize:(size_t)errorSize {
  return [self copyCVPixelBufferPresentFrameSet:frameSet
                                        overlay:nullptr
                                  toPixelBuffer:pixelBuffer
                                          width:width
                                         height:height
                                            out:out
                                          error:error
                                      errorSize:errorSize];
}

- (int)copyCVPixelBufferPresentFrameSet:(const VPMacOSNativeCVPixelBufferPresentFrameSet*)frameSet
                                overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                          toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                  width:(int32_t)width
                                 height:(int32_t)height
                                    out:(VPMacOSNativeFrameInfo*)out
                                  error:(char*)error
                              errorSize:(size_t)errorSize {
  if (![self isAvailable] || !_cvPixelBufferSetPipeline) {
    write_error(error, errorSize, "native Metal CVPixelBuffer set uploader is not available");
    return -1;
  }
  if (!frameSet || !pixelBuffer || !out || width <= 0 || height <= 0) {
    write_error(error, errorSize, "invalid native Metal CVPixelBuffer set upload arguments");
    return -1;
  }
  const int validationStatus =
      [self validatePixelBufferStatus:pixelBuffer width:width height:height];
  if (validationStatus != VPMacOSMetalUploaderStatusOk) {
    write_error(error, errorSize, VPMacOSMetalUploaderStatusMessageForCode(validationStatus));
    return -1;
  }
  if (![self ensureLayoutParamsBuffer]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal layout buffers");
  }

  auto* metalParams = static_cast<vp_macos::MetalLayoutParams*>([_layoutParamsBuffer contents]);
  vp_macos::fill_metal_layout_params(*metalParams, frameSet->decision, width, height);
  vp_macos::write_first_present_frame_info(frameSet->decision, out);

  std::array<vp_macos::ScopedCVMetalTexture, VPMacOSNativeMaxTracks> sourceYRefs;
  std::array<vp_macos::ScopedCVMetalTexture, VPMacOSNativeMaxTracks> sourceUVRefs;
  std::array<id<MTLTexture>, VPMacOSNativeMaxTracks> sourceYTextures = {};
  std::array<id<MTLTexture>, VPMacOSNativeMaxTracks> sourceUVTextures = {};
  int firstPresentSlot = -1;
  for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    if (!frameSet->decision.frames[slot].present) {
      continue;
    }
    CVPixelBufferRef sourcePixelBuffer =
        static_cast<CVPixelBufferRef>(frameSet->pixel_buffers[slot]);
    if (!sourcePixelBuffer || frameSet->plane_counts[slot] < 2 ||
        frameSet->coded_widths[slot] <= 0 || frameSet->coded_heights[slot] <= 0) {
      write_error(error, errorSize, "invalid native Metal CVPixelBuffer set frame");
      return -1;
    }
    const bool isP010 = frameSet->is_p010[slot] != 0;
    const MTLPixelFormat yFormat = isP010 ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm;
    const MTLPixelFormat uvFormat = isP010 ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm;
    const CVReturn yStatus = vp_macos::create_cv_metal_texture(
        _textureCache,
        sourcePixelBuffer,
        yFormat,
        CVPixelBufferGetWidthOfPlane(sourcePixelBuffer, 0),
        CVPixelBufferGetHeightOfPlane(sourcePixelBuffer, 0),
        0,
        &sourceYRefs[slot]);
    const CVReturn uvStatus = vp_macos::create_cv_metal_texture(
        _textureCache,
        sourcePixelBuffer,
        uvFormat,
        CVPixelBufferGetWidthOfPlane(sourcePixelBuffer, 1),
        CVPixelBufferGetHeightOfPlane(sourcePixelBuffer, 1),
        1,
        &sourceUVRefs[slot]);
    sourceYTextures[slot] = sourceYRefs[slot].texture();
    sourceUVTextures[slot] = sourceUVRefs[slot].texture();
    if (yStatus != kCVReturnSuccess || uvStatus != kCVReturnSuccess ||
        !sourceYTextures[slot] || !sourceUVTextures[slot]) {
      return metal_upload_failure(
          error, errorSize, "failed to wrap CVPixelBuffer set planes as Metal textures");
    }
    if (firstPresentSlot < 0) {
      firstPresentSlot = static_cast<int>(slot);
    }
  }
  if (firstPresentSlot < 0) {
    write_error(error, errorSize, "native Metal CVPixelBuffer set has no present frames");
    return -1;
  }
  for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    if (!sourceYTextures[slot]) {
      sourceYTextures[slot] = sourceYTextures[firstPresentSlot];
    }
    if (!sourceUVTextures[slot]) {
      sourceUVTextures[slot] = sourceUVTextures[firstPresentSlot];
    }
  }

  vp_macos::ScopedCVMetalTexture destinationRef;
  const CVReturn destinationStatus = vp_macos::create_cv_metal_texture(
      _textureCache,
      pixelBuffer,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &destinationRef);
  if (destinationStatus != kCVReturnSuccess || !destinationRef.valid()) {
    return metal_upload_failure(
        error, errorSize, "failed to wrap CVPixelBuffer destination as Metal texture");
  }

  id<MTLTexture> destinationTexture = destinationRef.texture();
  const auto gpuStart = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
  if (!destinationTexture || !commandBuffer || !compute) {
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal CVPixelBuffer set compute command");
  }

  [compute setComputePipelineState:_cvPixelBufferSetPipeline];
  [compute setBuffer:_layoutParamsBuffer offset:0 atIndex:0];
  [compute setTexture:destinationTexture atIndex:0];
  for (NSUInteger slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    [compute setTexture:sourceYTextures[slot] atIndex:(1 + slot * 2)];
    [compute setTexture:sourceUVTextures[slot] atIndex:(2 + slot * 2)];
  }

  const NSUInteger threadWidth = _cvPixelBufferSetPipeline.threadExecutionWidth;
  const NSUInteger threadHeight =
      std::max<NSUInteger>(1, _cvPixelBufferSetPipeline.maxTotalThreadsPerThreadgroup / threadWidth);
  const MTLSize threadsPerThreadgroup = MTLSizeMake(threadWidth, threadHeight, 1);
  const MTLSize threads = MTLSizeMake(width, height, 1);
  [compute dispatchThreads:threads threadsPerThreadgroup:threadsPerThreadgroup];
  [compute endEncoding];
  const int overlayRet = [self encodeOverlayGpuPrimitives:overlay
                                                 decision:&frameSet->decision
                                            commandBuffer:commandBuffer
                                       destinationTexture:destinationTexture
                                                    width:width
                                                   height:height
                                                    error:error
                                                errorSize:errorSize];
  if (overlayRet != 0) {
    return overlayRet;
  }
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  const BOOL completed = [commandBuffer status] == MTLCommandBufferStatusCompleted;
  _lastPresentPackageGpuWaitUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
  if (!completed) {
    return metal_upload_failure(
        error, errorSize, "native Metal CVPixelBuffer set compute did not complete");
  }

  _cvPixelBufferUploadCount.fetch_add(1, std::memory_order_relaxed);
  _lastPresentPackageCopyUs.store(0, std::memory_order_relaxed);
  _lastPresentPackageTotalUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
  _lastPresentPackageStorage.store(VPMacOSNativePresentPackageStorageCVPixelBuffer,
                                   std::memory_order_relaxed);
  write_error(error, errorSize, "");
  return 0;
}

- (int)compositeOverlayGpuRects:(const VPMacOSNativeOverlayGpuRect*)rects
                            count:(size_t)rectCount
                         decision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                    toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height
                            error:(char*)error
                        errorSize:(size_t)errorSize {
  return [self compositeOverlayGpuPrimitives:nil
                                   fillCount:0
                                   lineRects:rects
                                   lineCount:rectCount
                                 motionLines:nil
                                 motionCount:0
                                    decision:decisionInfo
                               toPixelBuffer:pixelBuffer
                                       width:width
                                      height:height
                                       error:error
                                   errorSize:errorSize];
}

- (int)encodeOverlayGpuPrimitives:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                          decision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                     commandBuffer:(id<MTLCommandBuffer>)commandBuffer
                destinationTexture:(id<MTLTexture>)destinationTexture
                             width:(int32_t)width
                            height:(int32_t)height
                             error:(char*)error
                         errorSize:(size_t)errorSize {
  if (!overlay_primitive_set_has_content(overlay)) {
    return 0;
  }
  const bool hasFillRects = overlay->fill_rect_count > 0;
  const bool hasLineRects = overlay->line_rect_count > 0;
  const bool hasMotionLines = overlay->motion_line_count > 0;
  if (![self isAvailable] ||
      (hasFillRects && !_overlayFillRectPipeline) ||
      (hasLineRects && (!_overlayLineMaskPipeline || !_overlayLineContrastPipeline)) ||
      (hasMotionLines && !_overlayMotionLinePipeline)) {
    write_error(error, errorSize, "native Metal overlay pipelines are not available");
    return -1;
  }
  if ((hasFillRects && !overlay->fill_rects) ||
      (hasLineRects && !overlay->line_rects) ||
      (hasMotionLines && !overlay->motion_lines) ||
      !decisionInfo || !commandBuffer || !destinationTexture ||
      width <= 0 || height <= 0) {
    write_error(error, errorSize, "invalid native Metal overlay arguments");
    return -1;
  }

  size_t fillRectBytes = 0;
  size_t rectBytes = 0;
  size_t motionLineBytes = 0;
  size_t maskBytes = 0;
  size_t maskPixels = 0;
  if (!checked_mul_size(static_cast<size_t>(width), static_cast<size_t>(height), &maskPixels) ||
      (hasFillRects &&
       (!checked_mul_size(overlay->fill_rect_count,
                          sizeof(VPMacOSNativeOverlayGpuRect),
                          &fillRectBytes) ||
        fillRectBytes == 0 ||
        ![self ensureOverlayFillRectBufferWithLength:fillRectBytes])) ||
      (hasLineRects &&
       (!checked_mul_size(maskPixels, sizeof(uint32_t), &maskBytes) ||
        !checked_mul_size(overlay->line_rect_count,
                          sizeof(VPMacOSNativeOverlayGpuRect),
                          &rectBytes) ||
        rectBytes == 0 ||
        maskBytes == 0 ||
        ![self ensureOverlayLineRectBufferWithLength:rectBytes] ||
        ![self ensureOverlayLineMaskBufferWithLength:maskBytes])) ||
      (hasMotionLines &&
       (!checked_mul_size(overlay->motion_line_count,
                          sizeof(VPMacOSNativeOverlayGpuRect),
                          &motionLineBytes) ||
        motionLineBytes == 0 ||
        ![self ensureOverlayMotionLineBufferWithLength:motionLineBytes])) ||
      ![self ensureLayoutParamsBuffer]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal overlay buffers");
  }
  if (hasFillRects) {
    std::memcpy([_overlayFillRectBuffer contents], overlay->fill_rects, fillRectBytes);
  }
  if (hasLineRects) {
    std::memcpy([_overlayLineRectBuffer contents], overlay->line_rects, rectBytes);
    std::memset([_overlayLineMaskBuffer contents], 0, maskBytes);
  }
  if (hasMotionLines) {
    std::memcpy([_overlayMotionLineBuffer contents], overlay->motion_lines, motionLineBytes);
  }
  auto* metalParams = static_cast<vp_macos::MetalLayoutParams*>([_layoutParamsBuffer contents]);
  vp_macos::fill_metal_layout_params(*metalParams, *decisionInfo, width, height);

  if (hasFillRects) {
    id<MTLComputeCommandEncoder> fillCompute = [commandBuffer computeCommandEncoder];
    if (!fillCompute) {
      return metal_upload_failure(
          error, errorSize, "failed to create native Metal overlay fill command");
    }
    [fillCompute setComputePipelineState:_overlayFillRectPipeline];
    [fillCompute setBuffer:_overlayFillRectBuffer offset:0 atIndex:0];
    [fillCompute setBuffer:_layoutParamsBuffer offset:0 atIndex:1];
    [fillCompute setTexture:destinationTexture atIndex:0];
    const NSUInteger fillThreadWidth = _overlayFillRectPipeline.threadExecutionWidth;
    const MTLSize fillThreadsPerThreadgroup = MTLSizeMake(fillThreadWidth, 1, 1);
    const MTLSize fillThreads = MTLSizeMake(overlay->fill_rect_count, 1, 1);
    [fillCompute dispatchThreads:fillThreads threadsPerThreadgroup:fillThreadsPerThreadgroup];
    [fillCompute endEncoding];
  }

  if (hasMotionLines) {
    id<MTLComputeCommandEncoder> motionCompute = [commandBuffer computeCommandEncoder];
    if (!motionCompute) {
      return metal_upload_failure(
          error, errorSize, "failed to create native Metal overlay motion command");
    }
    [motionCompute setComputePipelineState:_overlayMotionLinePipeline];
    [motionCompute setBuffer:_overlayMotionLineBuffer offset:0 atIndex:0];
    [motionCompute setBuffer:_layoutParamsBuffer offset:0 atIndex:1];
    [motionCompute setTexture:destinationTexture atIndex:0];
    const NSUInteger motionThreadWidth = _overlayMotionLinePipeline.threadExecutionWidth;
    const MTLSize motionThreadsPerThreadgroup = MTLSizeMake(motionThreadWidth, 1, 1);
    const MTLSize motionThreads = MTLSizeMake(overlay->motion_line_count, 1, 1);
    [motionCompute dispatchThreads:motionThreads threadsPerThreadgroup:motionThreadsPerThreadgroup];
    [motionCompute endEncoding];
  }

  if (hasLineRects) {
    id<MTLComputeCommandEncoder> maskCompute = [commandBuffer computeCommandEncoder];
    if (!maskCompute) {
      return metal_upload_failure(
          error, errorSize, "failed to create native Metal overlay line command");
    }
    [maskCompute setComputePipelineState:_overlayLineMaskPipeline];
    [maskCompute setBuffer:_overlayLineRectBuffer offset:0 atIndex:0];
    [maskCompute setBuffer:_overlayLineMaskBuffer offset:0 atIndex:1];
    [maskCompute setBuffer:_layoutParamsBuffer offset:0 atIndex:2];
    const NSUInteger maskThreadWidth = _overlayLineMaskPipeline.threadExecutionWidth;
    const MTLSize maskThreadsPerThreadgroup = MTLSizeMake(maskThreadWidth, 1, 1);
    const MTLSize maskThreads = MTLSizeMake(overlay->line_rect_count, 1, 1);
    [maskCompute dispatchThreads:maskThreads threadsPerThreadgroup:maskThreadsPerThreadgroup];
    [maskCompute endEncoding];

    id<MTLComputeCommandEncoder> contrastCompute = [commandBuffer computeCommandEncoder];
    if (!contrastCompute) {
      return metal_upload_failure(
          error, errorSize, "failed to create native Metal overlay contrast command");
    }
    [contrastCompute setComputePipelineState:_overlayLineContrastPipeline];
    [contrastCompute setBuffer:_overlayLineMaskBuffer offset:0 atIndex:0];
    [contrastCompute setBuffer:_layoutParamsBuffer offset:0 atIndex:1];
    [contrastCompute setTexture:destinationTexture atIndex:0];
    const NSUInteger contrastThreadWidth = _overlayLineContrastPipeline.threadExecutionWidth;
    const NSUInteger contrastThreadHeight =
        std::max<NSUInteger>(1,
                             _overlayLineContrastPipeline.maxTotalThreadsPerThreadgroup /
                                 contrastThreadWidth);
    const MTLSize contrastThreadsPerThreadgroup =
        MTLSizeMake(contrastThreadWidth, contrastThreadHeight, 1);
    const MTLSize contrastThreads = MTLSizeMake(width, height, 1);
    [contrastCompute dispatchThreads:contrastThreads
                threadsPerThreadgroup:contrastThreadsPerThreadgroup];
    [contrastCompute endEncoding];
  }
  return 0;
}

- (int)compositeOverlayGpuPrimitives:(const VPMacOSNativeOverlayGpuRect*)fillRects
                            fillCount:(size_t)fillRectCount
                            lineRects:(const VPMacOSNativeOverlayGpuRect*)lineRects
                            lineCount:(size_t)lineRectCount
                          motionLines:(const VPMacOSNativeOverlayGpuRect*)motionLines
                          motionCount:(size_t)motionLineCount
                             decision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
	                        toPixelBuffer:(CVPixelBufferRef)pixelBuffer
	                                width:(int32_t)width
	                               height:(int32_t)height
	                                error:(char*)error
	                            errorSize:(size_t)errorSize {
  const VPMacOSNativeOverlayGpuPrimitiveSet overlay = {
      fillRects,
      fillRectCount,
      lineRects,
      lineRectCount,
      motionLines,
      motionLineCount,
  };
  if (!overlay_primitive_set_has_content(&overlay)) {
    write_error(error, errorSize, "");
    return 0;
  }
  const int validationStatus =
      [self validatePixelBufferStatus:pixelBuffer width:width height:height];
  if (validationStatus != VPMacOSMetalUploaderStatusOk) {
    write_error(error, errorSize, VPMacOSMetalUploaderStatusMessageForCode(validationStatus));
    return -1;
  }

  vp_macos::ScopedCVMetalTexture destinationRef;
  const CVReturn destinationStatus = vp_macos::create_cv_metal_texture(
      _textureCache,
      pixelBuffer,
      MTLPixelFormatBGRA8Unorm,
      width,
      height,
      0,
      &destinationRef);
  if (destinationStatus != kCVReturnSuccess || !destinationRef.valid()) {
    return metal_upload_failure(
        error, errorSize, "failed to wrap CVPixelBuffer as a Metal overlay texture");
  }

  id<MTLTexture> destinationTexture = destinationRef.texture();
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  if (!destinationTexture || !commandBuffer) {
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal overlay compute command");
  }

  const int encodeRet = [self encodeOverlayGpuPrimitives:&overlay
                                                 decision:decisionInfo
                                            commandBuffer:commandBuffer
                                       destinationTexture:destinationTexture
                                                    width:width
                                                   height:height
                                                    error:error
                                                errorSize:errorSize];
  if (encodeRet != 0) {
    return encodeRet;
  }

  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  if ([commandBuffer status] != MTLCommandBufferStatusCompleted) {
    return metal_upload_failure(
        error, errorSize, "native Metal overlay compute did not complete");
  }
  write_error(error, errorSize, "");
  return 0;
}

@end
