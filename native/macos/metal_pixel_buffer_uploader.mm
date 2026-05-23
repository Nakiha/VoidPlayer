#include "native_player_bridge.h"

#include <CoreVideo/CoreVideo.h>
#include <Metal/Metal.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {

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

}  // namespace

@interface VPMacOSMetalUploaderImpl : NSObject {
 @private
  id<MTLDevice> _device;
  id<MTLCommandQueue> _commandQueue;
  id<MTLBuffer> _stagingBuffer;
  CVMetalTextureCacheRef _textureCache;
}

- (BOOL)isAvailable;
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

- (BOOL)validatePixelBuffer:(CVPixelBufferRef)pixelBuffer
                      width:(int32_t)width
                     height:(int32_t)height {
  if (![self isAvailable] || !pixelBuffer || width <= 0 || height <= 0) {
    return NO;
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
    return NO;
  }
  id<MTLTexture> texture = CVMetalTextureGetTexture(metalTextureRef);
  const BOOL valid = texture != nil;
  CFRelease(metalTextureRef);
  return valid;
}

- (BOOL)ensureStagingBufferWithLength:(size_t)length {
  if (_stagingBuffer != nil && [_stagingBuffer length] >= length) {
    return YES;
  }
  _stagingBuffer = [_device newBufferWithLength:length
                                        options:MTLResourceStorageModeShared];
  return _stagingBuffer != nil;
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
  if (!uploader || !uploader->impl) {
    return 0;
  }
  return [uploader->impl validatePixelBuffer:(CVPixelBufferRef)pixel_buffer
                                       width:width
                                      height:height] ? 1 : 0;
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
