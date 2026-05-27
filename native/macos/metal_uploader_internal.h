#ifndef VOIDPLAYER_MACOS_METAL_UPLOADER_INTERNAL_H_
#define VOIDPLAYER_MACOS_METAL_UPLOADER_INTERNAL_H_

#include "macos/metal_uploader_bridge.h"

#include <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>
#include <Metal/Metal.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

const char* VPMacOSMetalUploaderStatusMessageForCode(int status);

@interface VPMacOSMetalUploaderImpl : NSObject {
 @private
  id<MTLDevice> _device;
  id<MTLCommandQueue> _commandQueue;
  id<MTLBuffer> _stagingBuffer;
  id<MTLBuffer> _layoutParamsBuffer;
  id<MTLBuffer> _overlayLineRectBuffer;
  id<MTLBuffer> _overlayLineMaskBuffer;
  id<MTLComputePipelineState> _layoutPipeline;
  id<MTLComputePipelineState> _cvPixelBufferPipeline;
  id<MTLComputePipelineState> _overlayLineMaskPipeline;
  id<MTLComputePipelineState> _overlayLineContrastPipeline;
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
- (int)compositeOverlayLineRects:(const VPMacOSNativeOverlayLineRect*)rects
                            count:(size_t)rectCount
                    toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height
                            error:(char*)error
                        errorSize:(size_t)errorSize;

@end

struct VPMacOSMetalUploader {
  VPMacOSMetalUploaderImpl* impl;
};

#endif  // VOIDPLAYER_MACOS_METAL_UPLOADER_INTERNAL_H_
