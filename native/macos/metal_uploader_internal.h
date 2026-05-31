#ifndef VOIDPLAYER_MACOS_METAL_UPLOADER_INTERNAL_H_
#define VOIDPLAYER_MACOS_METAL_UPLOADER_INTERNAL_H_

#include "macos/metal_layout_params.h"
#include "macos/metal_uploader_bridge.h"

#include <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>
#include <Metal/Metal.h>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>

const char* VPMacOSMetalUploaderStatusMessageForCode(int status);

@interface VPMacOSMetalUploaderImpl : NSObject {
 @private
  id<MTLDevice> _device;
  id<MTLCommandQueue> _commandQueue;
  id<MTLBuffer> _stagingBuffer;
  id<MTLBuffer> _layoutParamsBuffer;
  id<MTLBuffer> _overlayFillRectBuffer;
  id<MTLBuffer> _overlayLineRectBuffer;
  id<MTLBuffer> _overlayMotionLineBuffer;
  id<MTLBuffer> _overlayDirectLineRectBuffer;
  id<MTLBuffer> _overlayLineMaskBuffer;
  id<MTLComputePipelineState> _layoutPipeline;
  id<MTLComputePipelineState> _cvPixelBufferPipeline;
  id<MTLComputePipelineState> _cvPixelBufferSetPipeline;
  id<MTLComputePipelineState> _overlayFillRectPipeline;
  id<MTLComputePipelineState> _overlayLineMaskPipeline;
  id<MTLComputePipelineState> _overlayLineContrastPipeline;
  id<MTLComputePipelineState> _overlayMotionLinePipeline;
  id<MTLComputePipelineState> _overlayLayerClearPipeline;
  id<MTLComputePipelineState> _overlayLayerFillRectPipeline;
  id<MTLComputePipelineState> _overlayLayerLineMaskPipeline;
  id<MTLComputePipelineState> _overlayLayerLineCompositePipeline;
  id<MTLComputePipelineState> _overlayLayerMotionLinePipeline;
  id<MTLComputePipelineState> _overlayDirectLinePipeline;
  id<MTLTexture> _transparentOverlayTexture;
  std::array<id<MTLTexture>, VPMacOSNativeMaxTracks> _overlayLayerTextures;
  std::array<uint64_t, VPMacOSNativeMaxTracks> _overlayLayerGenerations;
  std::array<int32_t, VPMacOSNativeMaxTracks> _overlayLayerWidths;
  std::array<int32_t, VPMacOSNativeMaxTracks> _overlayLayerHeights;
  uint64_t _overlayDirectLineRectGeneration;
  size_t _overlayDirectLineRectCount;
  size_t _overlayDirectLineRectBytes;
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
- (int)copyPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           data:(const uint8_t*)data
                       dataSize:(size_t)dataSize
                        overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                  toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                          width:(int32_t)width
                         height:(int32_t)height
                            out:(VPMacOSNativeFrameInfo*)out
                          error:(char*)error
                      errorSize:(size_t)errorSize;
- (int)copyPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           data:(const uint8_t*)data
                       dataSize:(size_t)dataSize
                        overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                  toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                          width:(int32_t)width
                         height:(int32_t)height
                            out:(VPMacOSNativeFrameInfo*)out
                          error:(char*)error
                      errorSize:(size_t)errorSize
                     completion:(VPMacOSMetalUploaderCompletion)completion
                       userData:(void*)userData;
- (int)copyCVPixelBufferPresentFrame:(const VPMacOSNativeCVPixelBufferPresentFrame*)frame
                        toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                width:(int32_t)width
                               height:(int32_t)height
                                  out:(VPMacOSNativeFrameInfo*)out
                                error:(char*)error
                            errorSize:(size_t)errorSize;
- (int)copyCVPixelBufferPresentFrame:(const VPMacOSNativeCVPixelBufferPresentFrame*)frame
                             overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                        toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                width:(int32_t)width
                               height:(int32_t)height
                                  out:(VPMacOSNativeFrameInfo*)out
                                error:(char*)error
                            errorSize:(size_t)errorSize;
- (int)copyCVPixelBufferPresentFrame:(const VPMacOSNativeCVPixelBufferPresentFrame*)frame
                             overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                        toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                width:(int32_t)width
                               height:(int32_t)height
                                  out:(VPMacOSNativeFrameInfo*)out
                                error:(char*)error
                            errorSize:(size_t)errorSize
                           completion:(VPMacOSMetalUploaderCompletion)completion
                             userData:(void*)userData;
- (int)copyCVPixelBufferPresentFrameSet:(const VPMacOSNativeCVPixelBufferPresentFrameSet*)frameSet
                          toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                  width:(int32_t)width
                                 height:(int32_t)height
                                    out:(VPMacOSNativeFrameInfo*)out
                                  error:(char*)error
                              errorSize:(size_t)errorSize;
- (int)copyCVPixelBufferPresentFrameSet:(const VPMacOSNativeCVPixelBufferPresentFrameSet*)frameSet
                                overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                          toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                  width:(int32_t)width
                                 height:(int32_t)height
                                    out:(VPMacOSNativeFrameInfo*)out
                                  error:(char*)error
                              errorSize:(size_t)errorSize;
- (int)copyCVPixelBufferPresentFrameSet:(const VPMacOSNativeCVPixelBufferPresentFrameSet*)frameSet
                                overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                          toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                  width:(int32_t)width
                                 height:(int32_t)height
                                    out:(VPMacOSNativeFrameInfo*)out
                                  error:(char*)error
                              errorSize:(size_t)errorSize
                             completion:(VPMacOSMetalUploaderCompletion)completion
                               userData:(void*)userData;
- (int)uploadPreparedPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                           toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                   width:(int32_t)width
                                  height:(int32_t)height
                                     out:(VPMacOSNativeFrameInfo*)out
                                   error:(char*)error
                               errorSize:(size_t)errorSize;
- (int)uploadPreparedPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                                 overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                           toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                   width:(int32_t)width
                                  height:(int32_t)height
                                     out:(VPMacOSNativeFrameInfo*)out
                                   error:(char*)error
                               errorSize:(size_t)errorSize;
- (int)uploadPreparedPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                                 overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                           toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                   width:(int32_t)width
                                  height:(int32_t)height
                                     out:(VPMacOSNativeFrameInfo*)out
                                   error:(char*)error
                               errorSize:(size_t)errorSize
                              completion:(VPMacOSMetalUploaderCompletion)completion
                                userData:(void*)userData;
- (int)compositeOverlayGpuRects:(const VPMacOSNativeOverlayGpuRect*)rects
                            count:(size_t)rectCount
                         decision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                    toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                            width:(int32_t)width
                           height:(int32_t)height
                            error:(char*)error
                        errorSize:(size_t)errorSize;
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
                            errorSize:(size_t)errorSize;
- (int)encodeOverlayGpuPrimitives:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                          decision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                     commandBuffer:(id<MTLCommandBuffer>)commandBuffer
                destinationTexture:(id<MTLTexture>)destinationTexture
                             width:(int32_t)width
                            height:(int32_t)height
                             error:(char*)error
	                         errorSize:(size_t)errorSize;
- (int)encodeDirectOverlayLinePrimitives:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                                decision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                           commandBuffer:(id<MTLCommandBuffer>)commandBuffer
                      destinationTexture:(id<MTLTexture>)destinationTexture
                                   width:(int32_t)width
                                  height:(int32_t)height
                                   error:(char*)error
                               errorSize:(size_t)errorSize;
- (BOOL)prepareOverlayLayers:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                     decision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                commandBuffer:(id<MTLCommandBuffer>)commandBuffer
                         error:(char*)error
                     errorSize:(size_t)errorSize;
- (void)bindOverlayLayersForDecision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                              overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                                params:(vp_macos::MetalLayoutParams*)params
                               encoder:(id<MTLComputeCommandEncoder>)encoder
                     firstTextureIndex:(NSUInteger)firstTextureIndex;

@end

struct VPMacOSMetalUploader {
  VPMacOSMetalUploaderImpl* impl;
};

#endif  // VOIDPLAYER_MACOS_METAL_UPLOADER_INTERNAL_H_
