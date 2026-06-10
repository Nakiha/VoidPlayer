#ifndef VOIDPLAYER_MACOS_METAL_UPLOADER_INTERNAL_H_
#define VOIDPLAYER_MACOS_METAL_UPLOADER_INTERNAL_H_

#include "macos/metal/metal_concurrency_policy.h"
#include "macos/metal/metal_layout_params.h"
#include "macos/metal/metal_uploader_bridge.h"

#include <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>
#include <Metal/Metal.h>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>

const char* VPMacOSMetalUploaderStatusMessageForCode(int status);

constexpr size_t VPMacOSMetalFrameResourcePoolSize =
    vp_macos::kMetalPresentConcurrencyPolicy.frame_resource_pool_size;

struct VPMacOSMetalFrameResources {
  std::atomic<bool> in_flight;
  id<MTLBuffer> staging_buffer;
  id<MTLBuffer> layout_params_buffer;
  id<MTLBuffer> overlay_direct_line_rect_buffer;
  uint64_t overlay_direct_line_rect_generation;
  size_t overlay_direct_line_rect_count;
  size_t overlay_direct_line_rect_bytes;

  VPMacOSMetalFrameResources()
      : in_flight(false),
        staging_buffer(nil),
        layout_params_buffer(nil),
        overlay_direct_line_rect_buffer(nil),
        overlay_direct_line_rect_generation(0),
        overlay_direct_line_rect_count(0),
        overlay_direct_line_rect_bytes(0) {}
};

struct VPMacOSMetalFrameResourcePool {
  std::array<VPMacOSMetalFrameResources, VPMacOSMetalFrameResourcePoolSize> slots;

  VPMacOSMetalFrameResources* tryAcquire() {
    for (auto& resource : slots) {
      bool expected = false;
      if (resource.in_flight.compare_exchange_strong(
              expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        return &resource;
      }
    }
    return nullptr;
  }
};

struct VPMacOSMetalPipelineRegistry {
  id<MTLLibrary> library = nil;
  id<MTLComputePipelineState> layout_package = nil;
  id<MTLComputePipelineState> layout_cv_single = nil;
  id<MTLComputePipelineState> layout_cv_set = nil;
  id<MTLComputePipelineState> overlay_legacy_fill = nil;
  id<MTLComputePipelineState> overlay_legacy_line_mask = nil;
  id<MTLComputePipelineState> overlay_legacy_line_contrast = nil;
  id<MTLComputePipelineState> overlay_legacy_motion = nil;
  id<MTLComputePipelineState> overlay_layer_clear = nil;
  id<MTLComputePipelineState> overlay_layer_fill_compute = nil;
  id<MTLComputePipelineState> overlay_layer_line_mask = nil;
  id<MTLComputePipelineState> overlay_layer_line_composite = nil;
  id<MTLComputePipelineState> overlay_layer_motion = nil;
  id<MTLComputePipelineState> overlay_direct_line = nil;
  id<MTLRenderPipelineState> overlay_layer_fill_render = nil;

  bool packagePathAvailable() const { return layout_package != nil; }
  bool cvSinglePathAvailable() const { return layout_cv_single != nil; }
  bool cvSetPathAvailable() const { return layout_cv_set != nil; }
  bool directLineOverlayAvailable() const { return overlay_direct_line != nil; }
  bool overlayLayerAvailable(bool needs_fill_rects, bool needs_motion_lines) const {
    return overlay_layer_clear != nil &&
        (!needs_fill_rects ||
         overlay_layer_fill_render != nil || overlay_layer_fill_compute != nil) &&
        (!needs_motion_lines || overlay_layer_motion != nil);
  }
  bool legacyOverlayAvailable(bool needs_fill_rects,
                              bool needs_line_rects,
                              bool needs_motion_lines) const {
    return (!needs_fill_rects || overlay_legacy_fill != nil) &&
        (!needs_line_rects ||
         (overlay_legacy_line_mask != nil && overlay_legacy_line_contrast != nil)) &&
        (!needs_motion_lines || overlay_legacy_motion != nil);
  }
};

@interface VPMacOSMetalUploaderImpl : NSObject {
 @private
  id<MTLDevice> _device;
  id<MTLCommandQueue> _commandQueue;
  id<MTLBuffer> _overlayFillRectBuffer;
  id<MTLBuffer> _overlayLineRectBuffer;
  id<MTLBuffer> _overlayMotionLineBuffer;
  id<MTLBuffer> _overlayLineMaskBuffer;
  VPMacOSMetalPipelineRegistry _pipelines;
  id<MTLTexture> _transparentOverlayTexture;
  std::array<id<MTLTexture>, VPMacOSNativeMaxTracks> _overlayLayerTextures;
  std::array<std::atomic<uint64_t>, VPMacOSNativeMaxTracks> _overlayLayerCommittedGenerations;
  std::array<std::atomic<uint64_t>, VPMacOSNativeMaxTracks> _overlayLayerPendingGenerations;
  std::array<int32_t, VPMacOSNativeMaxTracks> _overlayLayerWidths;
  std::array<int32_t, VPMacOSNativeMaxTracks> _overlayLayerHeights;
  CVMetalTextureCacheRef _textureCache;
  VPMacOSMetalFrameResourcePool _frameResourcePool;
  std::atomic<bool> _overlayLayerResourcesInFlight;
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
- (VPMacOSMetalFrameResources*)acquireFrameResourcesWithStagingLength:(size_t)stagingLength
                                                                error:(char*)error
                                                            errorSize:(size_t)errorSize;
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
                     frameResource:(VPMacOSMetalFrameResources*)frameResource
                             width:(int32_t)width
                            height:(int32_t)height
                             error:(char*)error
	                         errorSize:(size_t)errorSize;
- (int)encodeDirectOverlayLinePrimitives:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                                decision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                           commandBuffer:(id<MTLCommandBuffer>)commandBuffer
                      destinationTexture:(id<MTLTexture>)destinationTexture
                            frameResource:(VPMacOSMetalFrameResources*)frameResource
                                   width:(int32_t)width
                                  height:(int32_t)height
                                   error:(char*)error
                               errorSize:(size_t)errorSize;
- (BOOL)prepareOverlayLayers:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                     decision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                commandBuffer:(id<MTLCommandBuffer>)commandBuffer
           overlayResourceFlag:(std::atomic<bool>**)overlayResourceFlag
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
