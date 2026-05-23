#include "native_player_bridge.h"

#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {

constexpr const char* kLayoutBgraKernelSource = R"(
#include <metal_stdlib>
using namespace metal;

struct LayoutParams {
  uint width;
  uint height;
  float display_offset_x;
  float display_offset_y;
  float inv_display_size_x;
  float inv_display_size_y;
  float view_offset_uv_x;
  float view_offset_uv_y;
};

kernel void layout_bgra_copy(
    device const uchar4* source [[buffer(0)]],
    constant LayoutParams& params [[buffer(1)]],
    texture2d<float, access::write> destination [[texture(0)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }

  const float2 canvas_size = float2(float(params.width), float(params.height));
  const float2 local_uv = (float2(gid) + float2(0.5, 0.5)) / canvas_size;
  const float2 display_offset = float2(params.display_offset_x, params.display_offset_y);
  const float2 inv_display_size = float2(params.inv_display_size_x, params.inv_display_size_y);
  const float2 view_offset_uv = float2(params.view_offset_uv_x, params.view_offset_uv_y);
  const float2 source_uv = (local_uv - display_offset) * inv_display_size - view_offset_uv;

  if (source_uv.x < 0.0 || source_uv.x > 1.0 ||
      source_uv.y < 0.0 || source_uv.y > 1.0) {
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    return;
  }

  const uint source_x = min(uint(source_uv.x * float(params.width)), params.width - 1);
  const uint source_y = min(uint(source_uv.y * float(params.height)), params.height - 1);
  const uchar4 bgra = source[source_y * params.width + source_x];
  destination.write(
      float4(float(bgra.z), float(bgra.y), float(bgra.x), float(bgra.w)) / 255.0,
      gid);
}
)";

struct MetalLayoutParams {
  uint32_t width;
  uint32_t height;
  float display_offset_x;
  float display_offset_y;
  float inv_display_size_x;
  float inv_display_size_y;
  float view_offset_uv_x;
  float view_offset_uv_y;
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

}  // namespace

@interface VPMacOSMetalUploaderImpl : NSObject {
 @private
  id<MTLDevice> _device;
  id<MTLCommandQueue> _commandQueue;
  id<MTLBuffer> _stagingBuffer;
  id<MTLBuffer> _layoutParamsBuffer;
  id<MTLComputePipelineState> _layoutPipeline;
  CVMetalTextureCacheRef _textureCache;
}

- (BOOL)isAvailable;
- (int)validatePixelBufferStatus:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height;
- (BOOL)validatePixelBuffer:(CVPixelBufferRef)pixelBuffer
                      width:(int32_t)width
                     height:(int32_t)height;
- (int)copyCurrentFrameFromPlayer:(VPMacOSNativePlayer*)player
                    toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height
                    waitTimeoutMs:(int32_t)waitTimeoutMs
                              out:(VPMacOSNativeFrameInfo*)out
                            error:(char*)error
                        errorSize:(size_t)errorSize;
- (int)copyCurrentFrameWithLayoutFromPlayer:(VPMacOSNativePlayer*)player
                              toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                      width:(int32_t)width
                                     height:(int32_t)height
                              waitTimeoutMs:(int32_t)waitTimeoutMs
                                        out:(VPMacOSNativeFrameInfo*)out
                                      error:(char*)error
                                  errorSize:(size_t)errorSize;

@end

@implementation VPMacOSMetalUploaderImpl

- (instancetype)init {
  self = [super init];
  if (self) {
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

- (int)copyCurrentFrameFromPlayer:(VPMacOSNativePlayer*)player
                    toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height
                    waitTimeoutMs:(int32_t)waitTimeoutMs
                              out:(VPMacOSNativeFrameInfo*)out
                            error:(char*)error
                        errorSize:(size_t)errorSize {
  if (![self isAvailable]) {
    write_error(error, errorSize, "native Metal uploader is not available");
    return -1;
  }
  if (!player || !pixelBuffer || !out || width <= 0 || height <= 0) {
    write_error(error, errorSize, "invalid native Metal upload arguments");
    return -1;
  }
  const int validationStatus =
      [self validatePixelBufferStatus:pixelBuffer width:width height:height];
  if (validationStatus != VPMacOSMetalUploaderStatusOk) {
    write_error(error, errorSize, metal_uploader_status_message(validationStatus));
    return -1;
  }

  size_t rowBytes = 0;
  size_t uploadSize = 0;
  if (!checked_mul_size(static_cast<size_t>(width), 4u, &rowBytes) ||
      !checked_mul_size(rowBytes, static_cast<size_t>(height), &uploadSize)) {
    write_error(error, errorSize, "native Metal upload dimensions overflow");
    return -1;
  }
  if (rowBytes > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    write_error(error, errorSize, "native Metal upload row stride is too large");
    return -1;
  }
  if (![self ensureStagingBufferWithLength:uploadSize]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal staging buffer");
  }

  const int copyRet = VPMacOSNativePlayerCopyCurrentFrameBGRAInto(
      player,
      static_cast<uint8_t*>([_stagingBuffer contents]),
      uploadSize,
      width,
      height,
      static_cast<int32_t>(rowBytes),
      out,
      error,
      errorSize);
  if (copyRet != 0) {
    return copyRet;
  }

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
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
  if (!destinationTexture || !commandBuffer || !blit) {
    CFRelease(metalTextureRef);
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal blit command");
  }

  [blit copyFromBuffer:_stagingBuffer
          sourceOffset:0
     sourceBytesPerRow:rowBytes
   sourceBytesPerImage:uploadSize
            sourceSize:MTLSizeMake(width, height, 1)
             toTexture:destinationTexture
      destinationSlice:0
      destinationLevel:0
     destinationOrigin:MTLOriginMake(0, 0, 0)];
  [blit endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];

  const BOOL completed = [commandBuffer status] == MTLCommandBufferStatusCompleted;
  CFRelease(metalTextureRef);
  if (!completed) {
    return metal_upload_failure(
        error, errorSize, "native Metal blit did not complete");
  }

  write_error(error, errorSize, "");
  return 0;
}

- (int)copyCurrentFrameWithLayoutFromPlayer:(VPMacOSNativePlayer*)player
                              toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                      width:(int32_t)width
                                     height:(int32_t)height
                              waitTimeoutMs:(int32_t)waitTimeoutMs
                                        out:(VPMacOSNativeFrameInfo*)out
                                      error:(char*)error
                                  errorSize:(size_t)errorSize {
  if (![self isAvailable] || !_layoutPipeline) {
    write_error(error, errorSize, "native Metal layout uploader is not available");
    return -1;
  }
  if (!player || !pixelBuffer || !out || width <= 0 || height <= 0) {
    write_error(error, errorSize, "invalid native Metal layout upload arguments");
    return -1;
  }
  const int validationStatus =
      [self validatePixelBufferStatus:pixelBuffer width:width height:height];
  if (validationStatus != VPMacOSMetalUploaderStatusOk) {
    write_error(error, errorSize, metal_uploader_status_message(validationStatus));
    return -1;
  }

  size_t rowBytes = 0;
  size_t uploadSize = 0;
  if (!checked_mul_size(static_cast<size_t>(width), 4u, &rowBytes) ||
      !checked_mul_size(rowBytes, static_cast<size_t>(height), &uploadSize)) {
    write_error(error, errorSize, "native Metal layout upload dimensions overflow");
    return -1;
  }
  if (rowBytes > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    write_error(error, errorSize, "native Metal layout upload row stride is too large");
    return -1;
  }
  if (![self ensureStagingBufferWithLength:uploadSize] ||
      ![self ensureLayoutParamsBuffer]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal layout buffers");
  }

  const int copyRet = VPMacOSNativePlayerCopyCurrentFrameBGRAInto(
      player,
      static_cast<uint8_t*>([_stagingBuffer contents]),
      uploadSize,
      width,
      height,
      static_cast<int32_t>(rowBytes),
      out,
      error,
      errorSize);
  if (copyRet != 0) {
    return copyRet;
  }

  VPMacOSNativeLayoutPresentationParams layoutParams = {};
  if (VPMacOSNativePlayerCopyLayoutPresentationParams(
          player, width, height, &layoutParams) != 0) {
    write_error(error, errorSize, "failed to read native layout presentation parameters");
    return -1;
  }
  auto* metalParams = static_cast<MetalLayoutParams*>([_layoutParamsBuffer contents]);
  metalParams->width = static_cast<uint32_t>(width);
  metalParams->height = static_cast<uint32_t>(height);
  metalParams->display_offset_x = layoutParams.display_offset_x;
  metalParams->display_offset_y = layoutParams.display_offset_y;
  metalParams->inv_display_size_x = layoutParams.inv_display_size_x;
  metalParams->inv_display_size_y = layoutParams.inv_display_size_y;
  metalParams->view_offset_uv_x = layoutParams.view_offset_uv_x;
  metalParams->view_offset_uv_y = layoutParams.view_offset_uv_y;

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
  CFRelease(metalTextureRef);
  if (!completed) {
    return metal_upload_failure(
        error, errorSize, "native Metal layout compute did not complete");
  }

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

int VPMacOSMetalUploaderCopyCurrentFrame(VPMacOSMetalUploader* uploader,
                                         VPMacOSNativePlayer* player,
                                         void* pixel_buffer,
                                         int32_t width,
                                         int32_t height,
                                         int32_t wait_timeout_ms,
                                         VPMacOSNativeFrameInfo* out,
                                         char* error,
                                         size_t error_size) {
  if (!uploader || !uploader->impl) {
    write_error(error, error_size, "native Metal uploader is null");
    return -1;
  }
  return [uploader->impl copyCurrentFrameFromPlayer:player
                                     toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                             width:width
                                            height:height
                                     waitTimeoutMs:wait_timeout_ms
                                               out:out
                                             error:error
                                         errorSize:error_size];
}

int VPMacOSMetalUploaderCopyCurrentFrameWithLayout(
    VPMacOSMetalUploader* uploader,
    VPMacOSNativePlayer* player,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t wait_timeout_ms,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size) {
  if (!uploader || !uploader->impl) {
    write_error(error, error_size, "native Metal uploader is null");
    return -1;
  }
  return [uploader->impl copyCurrentFrameWithLayoutFromPlayer:player
                                                toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                                        width:width
                                                       height:height
                                                waitTimeoutMs:wait_timeout_ms
                                                          out:out
                                                        error:error
                                                    errorSize:error_size];
}
