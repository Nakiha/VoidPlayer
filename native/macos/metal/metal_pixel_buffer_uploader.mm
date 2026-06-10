#include "macos/metal/metal_uploader_internal.h"

#include "macos/metal/metal_layout_params.h"
#include "macos/metal/metal_texture_wrapping.h"

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr const char* kLayoutBgraKernelSource =
#include "macos/metal/generated/metal_pixel_buffer_uploader_shaders.inc"
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

id<MTLComputePipelineState> new_compute_pipeline(id<MTLDevice> device,
                                                 id<MTLLibrary> library,
                                                 NSString* function_name) {
  id<MTLFunction> function =
      library ? [library newFunctionWithName:function_name] : nil;
  if (!device || !function) {
    return nil;
  }
  NSError* pipelineError = nil;
  return [device newComputePipelineStateWithFunction:function error:&pipelineError];
}

id<MTLRenderPipelineState> new_overlay_fill_rect_render_pipeline(
    id<MTLDevice> device,
    id<MTLLibrary> library) {
  id<MTLFunction> vertexFunction =
      library ? [library newFunctionWithName:@"overlay_fill_rect_layer_vertex"] : nil;
  id<MTLFunction> fragmentFunction =
      library ? [library newFunctionWithName:@"overlay_fill_rect_layer_fragment"] : nil;
  if (!device || !vertexFunction || !fragmentFunction) {
    return nil;
  }
  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertexFunction;
  descriptor.fragmentFunction = fragmentFunction;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  descriptor.colorAttachments[0].blendingEnabled = YES;
  descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
  descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
  descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
  descriptor.colorAttachments[0].destinationRGBBlendFactor =
      MTLBlendFactorOneMinusSourceAlpha;
  descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
  descriptor.colorAttachments[0].destinationAlphaBlendFactor =
      MTLBlendFactorOneMinusSourceAlpha;
  NSError* pipelineError = nil;
  return [device newRenderPipelineStateWithDescriptor:descriptor
                                                error:&pipelineError];
}

VPMacOSMetalPipelineRegistry create_pipeline_registry(id<MTLDevice> device) {
  VPMacOSMetalPipelineRegistry registry;
  if (!device) {
    return registry;
  }
  NSError* libraryError = nil;
  NSString* source =
      [[NSString alloc] initWithUTF8String:kLayoutBgraKernelSource];
  registry.library = [device newLibraryWithSource:source
                                          options:nil
                                            error:&libraryError];
  registry.layout_package =
      new_compute_pipeline(device, registry.library, @"layout_bgra_copy");
  registry.layout_cv_single =
      new_compute_pipeline(device, registry.library, @"layout_cv_yuv_copy");
  registry.layout_cv_set =
      new_compute_pipeline(device, registry.library, @"layout_cv_yuv_set_copy");
  registry.overlay_legacy_fill =
      new_compute_pipeline(device, registry.library, @"composite_overlay_fill_rects");
  registry.overlay_legacy_line_mask =
      new_compute_pipeline(device, registry.library, @"build_overlay_line_mask");
  registry.overlay_legacy_line_contrast =
      new_compute_pipeline(device, registry.library, @"composite_overlay_line_contrast");
  registry.overlay_legacy_motion =
      new_compute_pipeline(device, registry.library, @"composite_overlay_motion_lines");
  registry.overlay_layer_clear =
      new_compute_pipeline(device, registry.library, @"clear_overlay_layer");
  registry.overlay_layer_fill_compute =
      new_compute_pipeline(device, registry.library, @"raster_overlay_fill_rects_layer");
  registry.overlay_layer_fill_render =
      new_overlay_fill_rect_render_pipeline(device, registry.library);
  registry.overlay_layer_line_mask =
      new_compute_pipeline(device, registry.library, @"build_overlay_line_mask_layer");
  registry.overlay_layer_line_composite =
      new_compute_pipeline(device, registry.library, @"composite_overlay_line_layer");
  registry.overlay_layer_motion =
      new_compute_pipeline(device, registry.library, @"raster_overlay_motion_lines_layer");
  registry.overlay_direct_line =
      new_compute_pipeline(device, registry.library, @"composite_overlay_line_rects_direct");
  return registry;
}

struct AsyncMetalResourceLifetime {
  std::vector<CVMetalTextureRef> textures;

  ~AsyncMetalResourceLifetime() {
    for (auto* texture : textures) {
      if (texture) {
        CFRelease(texture);
      }
    }
  }

  void retain(CVMetalTextureRef texture) {
    if (!texture) {
      return;
    }
    CFRetain(texture);
    textures.push_back(texture);
  }
};

void retain_async_texture(AsyncMetalResourceLifetime* lifetime,
                          const vp_macos::ScopedCVMetalTexture& texture) {
  if (lifetime) {
    lifetime->retain(texture.get());
  }
}

void release_resource_flag(std::atomic<bool>* inFlightFlag) {
  if (inFlightFlag) {
    inFlightFlag->store(false, std::memory_order_release);
  }
}

struct MetalResourceLease {
  std::atomic<bool>* frameResourceFlag = nullptr;
  std::atomic<bool>* overlayResourceFlag = nullptr;
};

void release_resource_lease(MetalResourceLease lease) {
  release_resource_flag(lease.frameResourceFlag);
  release_resource_flag(lease.overlayResourceFlag);
}

struct ScopedMetalResourceLease {
  explicit ScopedMetalResourceLease(MetalResourceLease lease)
      : lease_(lease) {}

  ~ScopedMetalResourceLease() { release_resource_lease(lease_); }

  void setOverlayResourceFlag(std::atomic<bool>* flag) {
    lease_.overlayResourceFlag = flag;
  }

  MetalResourceLease disarm() {
    MetalResourceLease lease = lease_;
    lease_ = {};
    return lease;
  }

 private:
  MetalResourceLease lease_;
};

class MetalCommandSubmitter {
public:
  static int submit(id<MTLCommandBuffer> commandBuffer,
                    std::chrono::steady_clock::time_point gpuStart,
                    VPMacOSNativeFrameInfo frameInfo,
                    const char* failureMessage,
                    std::atomic<int64_t>& lastGpuWaitUs,
                    std::atomic<int64_t>& lastTotalUs,
                    VPMacOSMetalUploaderCompletion completion,
                    void* userData,
                    AsyncMetalResourceLifetime* lifetime,
                    MetalResourceLease resourceLease,
                    char* error,
                    size_t errorSize) {
    if (!commandBuffer) {
      delete lifetime;
      release_resource_lease(resourceLease);
      return metal_upload_failure(error, errorSize, failureMessage);
    }
    if (completion) {
      auto* retainedLifetime = lifetime;
      const MetalResourceLease retainedResourceLease = resourceLease;
      [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
        const BOOL completed =
            [completedBuffer status] == MTLCommandBufferStatusCompleted;
        const int64_t gpuUs = elapsed_us_since(gpuStart);
        lastGpuWaitUs.store(gpuUs, std::memory_order_relaxed);
        lastTotalUs.store(gpuUs, std::memory_order_relaxed);
        std::string message;
        if (!completed) {
          message =
              failureMessage ? failureMessage : "native Metal command did not complete";
        }
        release_resource_lease(retainedResourceLease);
        completion(userData,
                   completed ? 0 : -2,
                   frameInfo,
                   message.c_str(),
                   gpuUs,
                   gpuUs);
        delete retainedLifetime;
      }];
      [commandBuffer commit];
      write_error(error, errorSize, "");
      return 0;
    }

    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    delete lifetime;
    release_resource_lease(resourceLease);

    const BOOL completed = [commandBuffer status] == MTLCommandBufferStatusCompleted;
    const int64_t gpuUs = elapsed_us_since(gpuStart);
    lastGpuWaitUs.store(gpuUs, std::memory_order_relaxed);
    lastTotalUs.store(gpuUs, std::memory_order_relaxed);
    if (!completed) {
      return metal_upload_failure(error, errorSize, failureMessage);
    }
    return 0;
  }
};

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

@interface VPMacOSMetalUploaderImpl ()
- (BOOL)ensureStagingBufferWithLength:(size_t)length
                              resource:(VPMacOSMetalFrameResources*)resource;
- (BOOL)ensureLayoutParamsBufferForResource:(VPMacOSMetalFrameResources*)resource;
- (BOOL)ensureDirectOverlayLineRectBuffer:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                                    bytes:(size_t)bytes
                                 resource:(VPMacOSMetalFrameResources*)resource;
- (int)uploadPreparedPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                                 overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                           toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                   width:(int32_t)width
                                  height:(int32_t)height
                                     out:(VPMacOSNativeFrameInfo*)out
                                   error:(char*)error
                               errorSize:(size_t)errorSize
                              completion:(VPMacOSMetalUploaderCompletion)completion
                                userData:(void*)userData
                           frameResource:(VPMacOSMetalFrameResources*)frameResource
                           resourceLease:(MetalResourceLease)resourceLease;
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
    _overlayLayerResourcesInFlight.store(false, std::memory_order_relaxed);
    for (auto& resource : _frameResourcePool.slots) {
      resource.in_flight.store(false, std::memory_order_relaxed);
      resource.overlay_direct_line_rect_generation = 0;
      resource.overlay_direct_line_rect_count = 0;
      resource.overlay_direct_line_rect_bytes = 0;
    }
    _overlayLayerTextures.fill(nil);
    for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
      _overlayLayerCommittedGenerations[slot].store(0, std::memory_order_relaxed);
      _overlayLayerPendingGenerations[slot].store(0, std::memory_order_relaxed);
    }
    _overlayLayerWidths.fill(0);
    _overlayLayerHeights.fill(0);
    _device = MTLCreateSystemDefaultDevice();
    if (_device) {
      _commandQueue = [_device newCommandQueue];
      CVMetalTextureCacheRef cache = nullptr;
      if (CVMetalTextureCacheCreate(
              kCFAllocatorDefault, nullptr, _device, nullptr, &cache) ==
          kCVReturnSuccess) {
        _textureCache = cache;
      }
      _pipelines = create_pipeline_registry(_device);
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
  return _device != nil && _commandQueue != nil && _textureCache != nullptr &&
      _pipelines.packagePathAvailable() &&
      _pipelines.cvSinglePathAvailable() &&
      _pipelines.cvSetPathAvailable();
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

- (VPMacOSMetalFrameResources*)acquireFrameResourcesWithStagingLength:(size_t)stagingLength
                                                                error:(char*)error
                                                            errorSize:(size_t)errorSize {
  VPMacOSMetalFrameResources* resource = _frameResourcePool.tryAcquire();
  if (resource) {
    if (![self ensureLayoutParamsBufferForResource:resource] ||
        (stagingLength > 0 &&
         ![self ensureStagingBufferWithLength:stagingLength resource:resource])) {
      release_resource_flag(&resource->in_flight);
      write_error(error, errorSize, "failed to allocate native Metal frame resources");
      return nullptr;
    }
    return resource;
  }
  write_error(error, errorSize, "native Metal uploader frame resource pool is busy");
  return nullptr;
}

- (BOOL)ensureStagingBufferWithLength:(size_t)length
                              resource:(VPMacOSMetalFrameResources*)resource {
  if (!resource) {
    return NO;
  }
  if (resource->staging_buffer != nil && [resource->staging_buffer length] >= length) {
    return YES;
  }
  resource->staging_buffer = [_device newBufferWithLength:length
                                                   options:MTLResourceStorageModeShared];
  return resource->staging_buffer != nil;
}

- (BOOL)ensureLayoutParamsBufferForResource:(VPMacOSMetalFrameResources*)resource {
  if (!resource) {
    return NO;
  }
  if (resource->layout_params_buffer != nil &&
      [resource->layout_params_buffer length] >= sizeof(vp_macos::MetalLayoutParams)) {
    return YES;
  }
  resource->layout_params_buffer =
      [_device newBufferWithLength:sizeof(vp_macos::MetalLayoutParams)
                            options:MTLResourceStorageModeShared];
  return resource->layout_params_buffer != nil;
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

- (BOOL)ensureDirectOverlayLineRectBuffer:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                                    bytes:(size_t)bytes
                                 resource:(VPMacOSMetalFrameResources*)resource {
  if (!overlay || overlay->line_rect_count == 0 || bytes == 0 || !overlay->line_rects) {
    return NO;
  }
  if (!resource) {
    return NO;
  }
  if (resource->overlay_direct_line_rect_buffer != nil &&
      resource->overlay_direct_line_rect_generation == overlay->generation &&
      resource->overlay_direct_line_rect_count == overlay->line_rect_count &&
      resource->overlay_direct_line_rect_bytes == bytes) {
    return YES;
  }
  resource->overlay_direct_line_rect_buffer =
      [_device newBufferWithBytes:overlay->line_rects
                           length:bytes
                          options:MTLResourceStorageModeShared];
  if (resource->overlay_direct_line_rect_buffer == nil) {
    resource->overlay_direct_line_rect_generation = 0;
    resource->overlay_direct_line_rect_count = 0;
    resource->overlay_direct_line_rect_bytes = 0;
    return NO;
  }
  resource->overlay_direct_line_rect_generation = overlay->generation;
  resource->overlay_direct_line_rect_count = overlay->line_rect_count;
  resource->overlay_direct_line_rect_bytes = bytes;
  return YES;
}

- (BOOL)ensureOverlayLineMaskBufferWithLength:(size_t)length {
  if (_overlayLineMaskBuffer != nil && [_overlayLineMaskBuffer length] >= length) {
    return YES;
  }
  _overlayLineMaskBuffer =
      [_device newBufferWithLength:length options:MTLResourceStorageModeShared];
  return _overlayLineMaskBuffer != nil;
}

- (BOOL)ensureTransparentOverlayTexture {
  if (_transparentOverlayTexture != nil) {
    return YES;
  }
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:1
                                                        height:1
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  _transparentOverlayTexture = [_device newTextureWithDescriptor:descriptor];
  if (!_transparentOverlayTexture) {
    return NO;
  }
  const uint8_t transparent[4] = {0, 0, 0, 0};
  [_transparentOverlayTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                                mipmapLevel:0
                                  withBytes:transparent
                                bytesPerRow:4];
  return YES;
}

- (BOOL)ensureOverlayLayerTextureForSlot:(NSUInteger)slot
                                   width:(int32_t)width
                                  height:(int32_t)height {
  if (slot >= VPMacOSNativeMaxTracks || width <= 0 || height <= 0) {
    return NO;
  }
  if (_overlayLayerTextures[slot] != nil &&
      _overlayLayerWidths[slot] == width &&
      _overlayLayerHeights[slot] == height) {
    return YES;
  }
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:static_cast<NSUInteger>(width)
                                                        height:static_cast<NSUInteger>(height)
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  descriptor.usage |= MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModePrivate;
  _overlayLayerTextures[slot] = [_device newTextureWithDescriptor:descriptor];
  _overlayLayerCommittedGenerations[slot].store(0, std::memory_order_release);
  _overlayLayerPendingGenerations[slot].store(0, std::memory_order_release);
  _overlayLayerWidths[slot] = _overlayLayerTextures[slot] ? width : 0;
  _overlayLayerHeights[slot] = _overlayLayerTextures[slot] ? height : 0;
  return _overlayLayerTextures[slot] != nil;
}

- (BOOL)prepareOverlayLayers:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                     decision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                commandBuffer:(id<MTLCommandBuffer>)commandBuffer
          overlayResourceFlag:(std::atomic<bool>**)overlayResourceFlag
                         error:(char*)error
                     errorSize:(size_t)errorSize {
  if (overlayResourceFlag) {
    *overlayResourceFlag = nullptr;
  }
  if (!overlay_primitive_set_has_content(overlay)) {
    return YES;
  }
  if (!decisionInfo || !commandBuffer || overlay->generation == 0) {
    write_error(error, errorSize, "invalid native Metal overlay layer arguments");
    return NO;
  }
  if (!_pipelines.overlayLayerAvailable(overlay->fill_rect_count > 0,
                                        overlay->motion_line_count > 0) ||
      ![self ensureTransparentOverlayTexture]) {
    write_error(error, errorSize, "native Metal overlay layer pipelines are not available");
    return NO;
  }

  std::array<bool, VPMacOSNativeMaxTracks> hasPrimitive = {};
  auto markPrimitiveSlots =
      [&](const VPMacOSNativeOverlayGpuRect* rects, size_t count) {
        for (size_t i = 0; i < count; ++i) {
          const uint32_t slot = rects[i].track_idx & 0xffu;
          if (slot < VPMacOSNativeMaxTracks) {
            hasPrimitive[slot] = true;
          }
        }
      };
  if (overlay->fill_rects && overlay->fill_rect_count > 0) {
    markPrimitiveSlots(overlay->fill_rects, overlay->fill_rect_count);
  }
  if (overlay->motion_lines && overlay->motion_line_count > 0) {
    markPrimitiveSlots(overlay->motion_lines, overlay->motion_line_count);
  }
  bool hasLayerPrimitive = false;
  for (bool value : hasPrimitive) {
    hasLayerPrimitive = hasLayerPrimitive || value;
  }

  std::array<bool, VPMacOSNativeMaxTracks> shouldRaster = {};
  bool anyRaster = false;
  for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    const int32_t width = decisionInfo->source_width[slot];
    const int32_t height = decisionInfo->source_height[slot];
    if (!hasPrimitive[slot] || !decisionInfo->frames[slot].present ||
        width <= 0 || height <= 0) {
      continue;
    }
    const bool cacheHit =
        _overlayLayerTextures[slot] != nil &&
        _overlayLayerCommittedGenerations[slot].load(std::memory_order_acquire) ==
            overlay->generation &&
        _overlayLayerWidths[slot] == width &&
        _overlayLayerHeights[slot] == height;
    shouldRaster[slot] = !cacheHit;
    anyRaster = anyRaster || shouldRaster[slot];
  }
  if (!anyRaster) {
    if (hasLayerPrimitive &&
        _overlayLayerResourcesInFlight.load(std::memory_order_acquire)) {
      write_error(error, errorSize, "native Metal uploader overlay layer resources are busy");
      return NO;
    }
    return YES;
  }
  bool expected = false;
  if (!_overlayLayerResourcesInFlight.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
    write_error(error, errorSize, "native Metal uploader overlay layer resources are busy");
    return NO;
  }
  ScopedMetalResourceLease overlayReservation({nullptr, &_overlayLayerResourcesInFlight});

  id<MTLBuffer> fillRectBuffer = nil;
  id<MTLBuffer> motionLineBuffer = nil;
  if (overlay->fill_rect_count > 0) {
    size_t bytes = 0;
    if (!overlay->fill_rects ||
        !checked_mul_size(overlay->fill_rect_count,
                          sizeof(VPMacOSNativeOverlayGpuRect),
                          &bytes)) {
      write_error(error, errorSize, "failed to allocate native Metal overlay layer fill buffer");
      return NO;
    }
    fillRectBuffer =
        [_device newBufferWithBytes:overlay->fill_rects
                              length:bytes
                             options:MTLResourceStorageModeShared];
    if (!fillRectBuffer) {
      write_error(error, errorSize, "failed to allocate native Metal overlay layer fill buffer");
      return NO;
    }
  }
  if (overlay->motion_line_count > 0) {
    size_t bytes = 0;
    if (!overlay->motion_lines ||
        !checked_mul_size(overlay->motion_line_count,
                          sizeof(VPMacOSNativeOverlayGpuRect),
                          &bytes)) {
      write_error(error, errorSize, "failed to allocate native Metal overlay layer motion buffer");
      return NO;
    }
    motionLineBuffer =
        [_device newBufferWithBytes:overlay->motion_lines
                              length:bytes
                             options:MTLResourceStorageModeShared];
    if (!motionLineBuffer) {
      write_error(error, errorSize, "failed to allocate native Metal overlay layer motion buffer");
      return NO;
    }
  }

  for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    if (!shouldRaster[slot]) {
      continue;
    }
    const int32_t width = decisionInfo->source_width[slot];
    const int32_t height = decisionInfo->source_height[slot];
    if (![self ensureOverlayLayerTextureForSlot:slot width:width height:height]) {
      write_error(error, errorSize, "failed to allocate native Metal overlay layer resources");
      return NO;
    }

    vp_macos::MetalOverlayLayerParams params = {};
    params.width = static_cast<uint32_t>(width);
    params.height = static_cast<uint32_t>(height);
    params.track_slot = static_cast<uint32_t>(slot);

    id<MTLTexture> layer = _overlayLayerTextures[slot];
    id<MTLComputeCommandEncoder> clearCompute = [commandBuffer computeCommandEncoder];
    if (!clearCompute) {
      write_error(error, errorSize, "failed to create native Metal overlay layer clear command");
      return NO;
    }
    [clearCompute setComputePipelineState:_pipelines.overlay_layer_clear];
    [clearCompute setTexture:layer atIndex:0];
    const NSUInteger clearThreadWidth = _pipelines.overlay_layer_clear.threadExecutionWidth;
    const NSUInteger clearThreadHeight =
        std::max<NSUInteger>(1,
                             _pipelines.overlay_layer_clear.maxTotalThreadsPerThreadgroup /
                                 clearThreadWidth);
    [clearCompute dispatchThreads:MTLSizeMake(width, height, 1)
             threadsPerThreadgroup:MTLSizeMake(clearThreadWidth, clearThreadHeight, 1)];
    [clearCompute endEncoding];

    if (overlay->fill_rect_count > 0) {
      size_t vertexCount = 0;
      if (!checked_mul_size(overlay->fill_rect_count, 6u, &vertexCount) ||
          vertexCount > std::numeric_limits<NSUInteger>::max()) {
        write_error(error, errorSize, "native Metal overlay layer fill vertex count overflow");
        return NO;
      }
      if (_pipelines.overlay_layer_fill_render) {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = layer;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> render = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (!render) {
          write_error(error, errorSize, "failed to create native Metal overlay layer fill render command");
          return NO;
        }
        [render setRenderPipelineState:_pipelines.overlay_layer_fill_render];
        [render setVertexBuffer:fillRectBuffer offset:0 atIndex:0];
        [render setVertexBytes:&params length:sizeof(params) atIndex:1];
        [render setViewport:MTLViewport{0.0, 0.0, static_cast<double>(width), static_cast<double>(height), 0.0, 1.0}];
        [render drawPrimitives:MTLPrimitiveTypeTriangle
                   vertexStart:0
                   vertexCount:static_cast<NSUInteger>(vertexCount)];
        [render endEncoding];
      } else {
        id<MTLComputeCommandEncoder> fillCompute = [commandBuffer computeCommandEncoder];
        if (!fillCompute) {
          write_error(error, errorSize, "failed to create native Metal overlay layer fill command");
          return NO;
        }
        [fillCompute setComputePipelineState:_pipelines.overlay_layer_fill_compute];
        [fillCompute setBuffer:fillRectBuffer offset:0 atIndex:0];
        [fillCompute setBytes:&params length:sizeof(params) atIndex:1];
        [fillCompute setTexture:layer atIndex:0];
        const NSUInteger fillThreadWidth = _pipelines.overlay_layer_fill_compute.threadExecutionWidth;
        [fillCompute dispatchThreads:MTLSizeMake(overlay->fill_rect_count, 1, 1)
                 threadsPerThreadgroup:MTLSizeMake(fillThreadWidth, 1, 1)];
        [fillCompute endEncoding];
      }
    }

    if (overlay->motion_line_count > 0) {
      id<MTLComputeCommandEncoder> motionCompute = [commandBuffer computeCommandEncoder];
      if (!motionCompute) {
        write_error(error, errorSize, "failed to create native Metal overlay layer motion command");
        return NO;
      }
      [motionCompute setComputePipelineState:_pipelines.overlay_layer_motion];
      [motionCompute setBuffer:motionLineBuffer offset:0 atIndex:0];
      [motionCompute setBytes:&params length:sizeof(params) atIndex:1];
      [motionCompute setTexture:layer atIndex:0];
      const NSUInteger motionThreadWidth = _pipelines.overlay_layer_motion.threadExecutionWidth;
      [motionCompute dispatchThreads:MTLSizeMake(overlay->motion_line_count, 1, 1)
                 threadsPerThreadgroup:MTLSizeMake(motionThreadWidth, 1, 1)];
      [motionCompute endEncoding];
    }

  }

  const uint64_t pendingGeneration = overlay->generation;
  std::array<bool, VPMacOSNativeMaxTracks> rasteredSlots = shouldRaster;
  for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    if (rasteredSlots[slot]) {
      _overlayLayerPendingGenerations[slot].store(pendingGeneration,
                                                  std::memory_order_release);
    }
  }
  [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completedBuffer) {
    const bool completed =
        [completedBuffer status] == MTLCommandBufferStatusCompleted;
    for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
      if (!rasteredSlots[slot]) {
        continue;
      }
      const uint64_t pending =
          _overlayLayerPendingGenerations[slot].load(std::memory_order_acquire);
      if (pending != pendingGeneration) {
        continue;
      }
      if (completed) {
        _overlayLayerCommittedGenerations[slot].store(pendingGeneration,
                                                      std::memory_order_release);
      }
      _overlayLayerPendingGenerations[slot].store(0, std::memory_order_release);
    }
  }];

  if (overlayResourceFlag) {
    *overlayResourceFlag = &_overlayLayerResourcesInFlight;
  }
  overlayReservation.disarm();
  return YES;
}

- (void)bindOverlayLayersForDecision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                              overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                                params:(vp_macos::MetalLayoutParams*)params
                               encoder:(id<MTLComputeCommandEncoder>)encoder
                     firstTextureIndex:(NSUInteger)firstTextureIndex {
  uint32_t overlayPresent[VPMacOSNativeMaxTracks] = {};
  const bool hasOverlay = overlay_primitive_set_has_content(overlay) &&
      overlay->generation != 0;
  if (hasOverlay && decisionInfo) {
    for (size_t slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
      const int32_t width = decisionInfo->source_width[slot];
      const int32_t height = decisionInfo->source_height[slot];
      if (decisionInfo->frames[slot].present &&
          _overlayLayerTextures[slot] != nil &&
          (_overlayLayerCommittedGenerations[slot].load(std::memory_order_acquire) ==
               overlay->generation ||
           _overlayLayerPendingGenerations[slot].load(std::memory_order_acquire) ==
               overlay->generation) &&
          _overlayLayerWidths[slot] == width &&
          _overlayLayerHeights[slot] == height) {
        overlayPresent[slot] = 1;
      }
    }
  }
  if (params) {
    vp_macos::set_metal_overlay_present(*params, overlayPresent);
  }
  [self ensureTransparentOverlayTexture];
  for (NSUInteger slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    id<MTLTexture> texture = overlayPresent[slot] ? _overlayLayerTextures[slot] : nil;
    if (!texture) {
      texture = _transparentOverlayTexture;
    }
    [encoder setTexture:texture atIndex:firstTextureIndex + slot];
  }
}

- (int)encodeDirectOverlayLinePrimitives:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                                decision:(const VPMacOSNativePresentDecisionInfo*)decisionInfo
                           commandBuffer:(id<MTLCommandBuffer>)commandBuffer
                      destinationTexture:(id<MTLTexture>)destinationTexture
                            frameResource:(VPMacOSMetalFrameResources*)frameResource
                                   width:(int32_t)width
                                  height:(int32_t)height
                                   error:(char*)error
                               errorSize:(size_t)errorSize {
  if (!overlay || overlay->line_rect_count == 0) {
    return 0;
  }
  if (![self isAvailable] || !_pipelines.directLineOverlayAvailable() || !frameResource ||
      !frameResource->layout_params_buffer) {
    write_error(error, errorSize, "native Metal overlay direct line pipeline is not available");
    return -1;
  }
  if (!overlay->line_rects || !decisionInfo || !commandBuffer || !destinationTexture ||
      width <= 0 || height <= 0) {
    write_error(error, errorSize, "invalid native Metal overlay direct line arguments");
    return -1;
  }
  size_t lineBytes = 0;
  if (!checked_mul_size(overlay->line_rect_count,
                        sizeof(VPMacOSNativeOverlayGpuRect),
                        &lineBytes) ||
      lineBytes == 0 ||
      ![self ensureDirectOverlayLineRectBuffer:overlay
                                         bytes:lineBytes
                                      resource:frameResource]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal overlay direct line buffer");
  }

  auto encodePass = [&](uint32_t pass) -> int {
    vp_macos::MetalOverlayLinePassParams passParams = {};
    passParams.width = static_cast<uint32_t>(width);
    passParams.height = static_cast<uint32_t>(height);
    passParams.pass = pass;
    id<MTLComputeCommandEncoder> lineCompute = [commandBuffer computeCommandEncoder];
    if (!lineCompute) {
      return metal_upload_failure(
          error, errorSize, "failed to create native Metal overlay direct line command");
    }
    [lineCompute setComputePipelineState:_pipelines.overlay_direct_line];
    [lineCompute setBuffer:frameResource->overlay_direct_line_rect_buffer offset:0 atIndex:0];
    [lineCompute setBuffer:frameResource->layout_params_buffer offset:0 atIndex:1];
    [lineCompute setBytes:&passParams length:sizeof(passParams) atIndex:2];
    [lineCompute setTexture:destinationTexture atIndex:0];
    const NSUInteger lineThreadWidth = _pipelines.overlay_direct_line.threadExecutionWidth;
    [lineCompute dispatchThreads:MTLSizeMake(overlay->line_rect_count, 1, 1)
             threadsPerThreadgroup:MTLSizeMake(lineThreadWidth, 1, 1)];
    [lineCompute endEncoding];
    return 0;
  };

  const int borderRet = encodePass(0);
  if (borderRet != 0) {
    return borderRet;
  }
  return encodePass(1);
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
  return [self copyPresentFramePackage:package
                                  data:data
                              dataSize:dataSize
                               overlay:overlay
                         toPixelBuffer:pixelBuffer
                                 width:width
                                height:height
                                   out:out
                                 error:error
                             errorSize:errorSize
                            completion:nullptr
                              userData:nullptr];
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
                      errorSize:(size_t)errorSize
                     completion:(VPMacOSMetalUploaderCompletion)completion
                       userData:(void*)userData {
  if (![self isAvailable] || !_pipelines.packagePathAvailable()) {
    write_error(error, errorSize, "native Metal layout uploader is not available");
    return -1;
  }
  if (!package || !data || dataSize == 0 || package->used_bytes == 0 ||
      package->used_bytes > dataSize) {
    write_error(error, errorSize, "invalid native Metal present package arguments");
    return -1;
  }
  VPMacOSMetalFrameResources* frameResource =
      [self acquireFrameResourcesWithStagingLength:package->used_bytes
                                             error:error
                                         errorSize:errorSize];
  if (!frameResource) {
    return -2;
  }
  ScopedMetalResourceLease reservation({&frameResource->in_flight, nullptr});
  const auto totalStart = std::chrono::steady_clock::now();
  const auto copyStart = std::chrono::steady_clock::now();
  std::memcpy([frameResource->staging_buffer contents], data, package->used_bytes);
  _lastPresentPackageCopyUs.store(elapsed_us_since(copyStart), std::memory_order_relaxed);
  const int uploadRet = [self uploadPreparedPresentFramePackage:package
                                                       overlay:overlay
                                                  toPixelBuffer:pixelBuffer
                                                          width:width
                                                         height:height
                                                            out:out
                                                          error:error
                                                      errorSize:errorSize
                                                     completion:completion
                                                       userData:userData
                                                  frameResource:frameResource
                                          resourceLease:reservation.disarm()];
  if (!completion) {
    _lastPresentPackageTotalUs.store(elapsed_us_since(totalStart), std::memory_order_relaxed);
  }
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
  return [self uploadPreparedPresentFramePackage:package
                                         overlay:overlay
                                   toPixelBuffer:pixelBuffer
                                           width:width
                                          height:height
                                             out:out
                                           error:error
                                       errorSize:errorSize
                                      completion:nullptr
                                        userData:nullptr];
}

- (int)uploadPreparedPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                                 overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                           toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                   width:(int32_t)width
                                  height:(int32_t)height
                                     out:(VPMacOSNativeFrameInfo*)out
                                   error:(char*)error
                               errorSize:(size_t)errorSize
                              completion:(VPMacOSMetalUploaderCompletion)completion
                                userData:(void*)userData {
  (void)package;
  (void)overlay;
  (void)pixelBuffer;
  (void)width;
  (void)height;
  (void)out;
  (void)completion;
  (void)userData;
  write_error(
      error,
      errorSize,
      "prepared native Metal package upload is disabled; pass package bytes explicitly");
  return -1;
}

- (int)uploadPreparedPresentFramePackage:(const VPMacOSNativePresentFramePackageInfo*)package
                                 overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                           toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                   width:(int32_t)width
                                  height:(int32_t)height
                                     out:(VPMacOSNativeFrameInfo*)out
                                   error:(char*)error
                               errorSize:(size_t)errorSize
                              completion:(VPMacOSMetalUploaderCompletion)completion
                                userData:(void*)userData
                            frameResource:(VPMacOSMetalFrameResources*)frameResource
                            resourceLease:(MetalResourceLease)resourceLease {
  ScopedMetalResourceLease reservation(resourceLease);
  if (![self isAvailable] || !_pipelines.packagePathAvailable()) {
    write_error(error, errorSize, "native Metal layout uploader is not available");
    return -1;
  }
  if (!package || !pixelBuffer || width <= 0 || height <= 0 ||
      package->storage == VPMacOSNativePresentPackageStorageUnavailable ||
      !frameResource || !frameResource->staging_buffer ||
      !frameResource->layout_params_buffer) {
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

  auto* metalParams =
      static_cast<vp_macos::MetalLayoutParams*>([frameResource->layout_params_buffer contents]);
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
  auto* lifetime = completion ? new AsyncMetalResourceLifetime() : nullptr;
  retain_async_texture(lifetime, destinationRef);

  id<MTLTexture> destinationTexture = destinationRef.texture();
  const auto gpuStart = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  if (!destinationTexture || !commandBuffer) {
    delete lifetime;
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal layout compute command");
  }
  std::atomic<bool>* overlayResourceFlag = nullptr;
  if (![self prepareOverlayLayers:overlay
                          decision:&package->decision
                     commandBuffer:commandBuffer
              overlayResourceFlag:&overlayResourceFlag
                              error:error
                          errorSize:errorSize]) {
    delete lifetime;
    return -2;
  }
  reservation.setOverlayResourceFlag(overlayResourceFlag);

  id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
  if (!compute) {
    delete lifetime;
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal layout compute command");
  }

  [compute setComputePipelineState:_pipelines.layout_package];
  [compute setBuffer:frameResource->staging_buffer offset:0 atIndex:0];
  [compute setBuffer:frameResource->layout_params_buffer offset:0 atIndex:1];
  [compute setTexture:destinationTexture atIndex:0];
  [self bindOverlayLayersForDecision:&package->decision
                              overlay:overlay
                                params:metalParams
                               encoder:compute
                     firstTextureIndex:1];

  const NSUInteger threadWidth = _pipelines.layout_package.threadExecutionWidth;
  const NSUInteger threadHeight =
      std::max<NSUInteger>(1, _pipelines.layout_package.maxTotalThreadsPerThreadgroup / threadWidth);
  const MTLSize threadsPerThreadgroup = MTLSizeMake(threadWidth, threadHeight, 1);
  const MTLSize threads = MTLSizeMake(width, height, 1);
  [compute dispatchThreads:threads threadsPerThreadgroup:threadsPerThreadgroup];
  [compute endEncoding];
  const int lineRet = [self encodeDirectOverlayLinePrimitives:overlay
                                                     decision:&package->decision
                                                commandBuffer:commandBuffer
                                           destinationTexture:destinationTexture
                                                frameResource:frameResource
                                                        width:width
                                                       height:height
                                                        error:error
                                                    errorSize:errorSize];
  if (lineRet != 0) {
    delete lifetime;
    return lineRet;
  }
  const int commitRet = MetalCommandSubmitter::submit(commandBuffer,
                                            gpuStart,
                                            out ? *out : VPMacOSNativeFrameInfo{},
                                            "native Metal layout compute did not complete",
                                            _lastPresentPackageGpuWaitUs,
                                            _lastPresentPackageTotalUs,
                                            completion,
                                            userData,
                                            lifetime,
                                            reservation.disarm(),
                                            error,
                                            errorSize);
  if (commitRet != 0) {
    return commitRet;
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
  return [self copyCVPixelBufferPresentFrame:frame
                                     overlay:overlay
                               toPixelBuffer:pixelBuffer
                                       width:width
                                      height:height
                                         out:out
                                       error:error
                                   errorSize:errorSize
                                  completion:nullptr
                                    userData:nullptr];
}

- (int)copyCVPixelBufferPresentFrame:(const VPMacOSNativeCVPixelBufferPresentFrame*)frame
                             overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                        toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                width:(int32_t)width
                               height:(int32_t)height
                                  out:(VPMacOSNativeFrameInfo*)out
                                error:(char*)error
                            errorSize:(size_t)errorSize
                           completion:(VPMacOSMetalUploaderCompletion)completion
                             userData:(void*)userData {
  if (![self isAvailable] || !_pipelines.cvSinglePathAvailable()) {
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
  VPMacOSMetalFrameResources* frameResource =
      [self acquireFrameResourcesWithStagingLength:0 error:error errorSize:errorSize];
  if (!frameResource) {
    return -2;
  }
  ScopedMetalResourceLease reservation({&frameResource->in_flight, nullptr});

  auto* metalParams =
      static_cast<vp_macos::MetalLayoutParams*>([frameResource->layout_params_buffer contents]);
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
  auto* lifetime = completion ? new AsyncMetalResourceLifetime() : nullptr;
  retain_async_texture(lifetime, sourceYRef);
  retain_async_texture(lifetime, sourceUVRef);
  retain_async_texture(lifetime, destinationRef);

  id<MTLTexture> sourceYTexture = sourceYRef.texture();
  id<MTLTexture> sourceUVTexture = sourceUVRef.texture();
  id<MTLTexture> destinationTexture = destinationRef.texture();
  const auto gpuStart = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  if (!sourceYTexture || !sourceUVTexture || !destinationTexture ||
      !commandBuffer) {
    delete lifetime;
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal CVPixelBuffer compute command");
  }
  std::atomic<bool>* overlayResourceFlag = nullptr;
  if (![self prepareOverlayLayers:overlay
                          decision:&frame->decision
                     commandBuffer:commandBuffer
              overlayResourceFlag:&overlayResourceFlag
                              error:error
                          errorSize:errorSize]) {
    delete lifetime;
    return -2;
  }
  reservation.setOverlayResourceFlag(overlayResourceFlag);

  id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
  if (!compute) {
    delete lifetime;
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal CVPixelBuffer compute command");
  }

  [compute setComputePipelineState:_pipelines.layout_cv_single];
  [compute setBuffer:frameResource->layout_params_buffer offset:0 atIndex:0];
  [compute setTexture:destinationTexture atIndex:0];
  [compute setTexture:sourceYTexture atIndex:1];
  [compute setTexture:sourceUVTexture atIndex:2];
  [self bindOverlayLayersForDecision:&frame->decision
                              overlay:overlay
                                params:metalParams
                               encoder:compute
                     firstTextureIndex:3];

  const NSUInteger threadWidth = _pipelines.layout_cv_single.threadExecutionWidth;
  const NSUInteger threadHeight =
      std::max<NSUInteger>(1, _pipelines.layout_cv_single.maxTotalThreadsPerThreadgroup / threadWidth);
  const MTLSize threadsPerThreadgroup = MTLSizeMake(threadWidth, threadHeight, 1);
  const MTLSize threads = MTLSizeMake(width, height, 1);
  [compute dispatchThreads:threads threadsPerThreadgroup:threadsPerThreadgroup];
  [compute endEncoding];
  const int lineRet = [self encodeDirectOverlayLinePrimitives:overlay
                                                     decision:&frame->decision
                                                commandBuffer:commandBuffer
                                           destinationTexture:destinationTexture
                                                frameResource:frameResource
                                                        width:width
                                                       height:height
                                                        error:error
                                                    errorSize:errorSize];
  if (lineRet != 0) {
    delete lifetime;
    return lineRet;
  }
  const int commitRet = MetalCommandSubmitter::submit(commandBuffer,
                                            gpuStart,
                                            out ? *out : VPMacOSNativeFrameInfo{},
                                            "native Metal CVPixelBuffer compute did not complete",
                                            _lastPresentPackageGpuWaitUs,
                                            _lastPresentPackageTotalUs,
                                            completion,
                                            userData,
                                            lifetime,
                                            reservation.disarm(),
                                            error,
                                            errorSize);
  if (commitRet != 0) {
    return commitRet;
  }

  _cvPixelBufferUploadCount.fetch_add(1, std::memory_order_relaxed);
  _lastPresentPackageCopyUs.store(0, std::memory_order_relaxed);
  if (!completion) {
    _lastPresentPackageTotalUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
  }
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
  return [self copyCVPixelBufferPresentFrameSet:frameSet
                                        overlay:overlay
                                  toPixelBuffer:pixelBuffer
                                          width:width
                                         height:height
                                            out:out
                                          error:error
                                      errorSize:errorSize
                                     completion:nullptr
                                       userData:nullptr];
}

- (int)copyCVPixelBufferPresentFrameSet:(const VPMacOSNativeCVPixelBufferPresentFrameSet*)frameSet
                                overlay:(const VPMacOSNativeOverlayGpuPrimitiveSet*)overlay
                          toPixelBuffer:(CVPixelBufferRef)pixelBuffer
                                  width:(int32_t)width
                                 height:(int32_t)height
                                    out:(VPMacOSNativeFrameInfo*)out
                                  error:(char*)error
                              errorSize:(size_t)errorSize
                             completion:(VPMacOSMetalUploaderCompletion)completion
                               userData:(void*)userData {
  if (![self isAvailable] || !_pipelines.cvSetPathAvailable()) {
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
  VPMacOSMetalFrameResources* frameResource =
      [self acquireFrameResourcesWithStagingLength:0 error:error errorSize:errorSize];
  if (!frameResource) {
    return -2;
  }
  ScopedMetalResourceLease reservation({&frameResource->in_flight, nullptr});

  auto* metalParams =
      static_cast<vp_macos::MetalLayoutParams*>([frameResource->layout_params_buffer contents]);
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
  auto* lifetime = completion ? new AsyncMetalResourceLifetime() : nullptr;
  for (auto& ref : sourceYRefs) {
    retain_async_texture(lifetime, ref);
  }
  for (auto& ref : sourceUVRefs) {
    retain_async_texture(lifetime, ref);
  }
  retain_async_texture(lifetime, destinationRef);

  id<MTLTexture> destinationTexture = destinationRef.texture();
  const auto gpuStart = std::chrono::steady_clock::now();
  id<MTLCommandBuffer> commandBuffer = [_commandQueue commandBuffer];
  if (!destinationTexture || !commandBuffer) {
    delete lifetime;
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal CVPixelBuffer set compute command");
  }
  std::atomic<bool>* overlayResourceFlag = nullptr;
  if (![self prepareOverlayLayers:overlay
                          decision:&frameSet->decision
                     commandBuffer:commandBuffer
              overlayResourceFlag:&overlayResourceFlag
                              error:error
                          errorSize:errorSize]) {
    delete lifetime;
    return -2;
  }
  reservation.setOverlayResourceFlag(overlayResourceFlag);

  id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
  if (!compute) {
    delete lifetime;
    return metal_upload_failure(
        error, errorSize, "failed to create native Metal CVPixelBuffer set compute command");
  }

  [compute setComputePipelineState:_pipelines.layout_cv_set];
  [compute setBuffer:frameResource->layout_params_buffer offset:0 atIndex:0];
  [compute setTexture:destinationTexture atIndex:0];
  for (NSUInteger slot = 0; slot < VPMacOSNativeMaxTracks; ++slot) {
    [compute setTexture:sourceYTextures[slot] atIndex:(1 + slot * 2)];
    [compute setTexture:sourceUVTextures[slot] atIndex:(2 + slot * 2)];
  }
  [self bindOverlayLayersForDecision:&frameSet->decision
                              overlay:overlay
                                params:metalParams
                               encoder:compute
                     firstTextureIndex:9];

  const NSUInteger threadWidth = _pipelines.layout_cv_set.threadExecutionWidth;
  const NSUInteger threadHeight =
      std::max<NSUInteger>(1, _pipelines.layout_cv_set.maxTotalThreadsPerThreadgroup / threadWidth);
  const MTLSize threadsPerThreadgroup = MTLSizeMake(threadWidth, threadHeight, 1);
  const MTLSize threads = MTLSizeMake(width, height, 1);
  [compute dispatchThreads:threads threadsPerThreadgroup:threadsPerThreadgroup];
  [compute endEncoding];
  const int lineRet = [self encodeDirectOverlayLinePrimitives:overlay
                                                     decision:&frameSet->decision
                                                commandBuffer:commandBuffer
                                           destinationTexture:destinationTexture
                                                frameResource:frameResource
                                                        width:width
                                                       height:height
                                                        error:error
                                                    errorSize:errorSize];
  if (lineRet != 0) {
    delete lifetime;
    return lineRet;
  }
  const int commitRet = MetalCommandSubmitter::submit(
      commandBuffer,
      gpuStart,
      out ? *out : VPMacOSNativeFrameInfo{},
      "native Metal CVPixelBuffer set compute did not complete",
      _lastPresentPackageGpuWaitUs,
      _lastPresentPackageTotalUs,
      completion,
      userData,
      lifetime,
      reservation.disarm(),
      error,
      errorSize);
  if (commitRet != 0) {
    return commitRet;
  }

  _cvPixelBufferUploadCount.fetch_add(1, std::memory_order_relaxed);
  _lastPresentPackageCopyUs.store(0, std::memory_order_relaxed);
  if (!completion) {
    _lastPresentPackageTotalUs.store(elapsed_us_since(gpuStart), std::memory_order_relaxed);
  }
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
                     frameResource:(VPMacOSMetalFrameResources*)frameResource
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
      !_pipelines.legacyOverlayAvailable(hasFillRects, hasLineRects, hasMotionLines)) {
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
      !frameResource ||
      !frameResource->layout_params_buffer) {
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
  auto* metalParams =
      static_cast<vp_macos::MetalLayoutParams*>([frameResource->layout_params_buffer contents]);
  vp_macos::fill_metal_layout_params(*metalParams, *decisionInfo, width, height);

  if (hasFillRects) {
    id<MTLComputeCommandEncoder> fillCompute = [commandBuffer computeCommandEncoder];
    if (!fillCompute) {
      return metal_upload_failure(
          error, errorSize, "failed to create native Metal overlay fill command");
    }
    [fillCompute setComputePipelineState:_pipelines.overlay_legacy_fill];
    [fillCompute setBuffer:_overlayFillRectBuffer offset:0 atIndex:0];
    [fillCompute setBuffer:frameResource->layout_params_buffer offset:0 atIndex:1];
    [fillCompute setTexture:destinationTexture atIndex:0];
    const NSUInteger fillThreadWidth = _pipelines.overlay_legacy_fill.threadExecutionWidth;
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
    [motionCompute setComputePipelineState:_pipelines.overlay_legacy_motion];
    [motionCompute setBuffer:_overlayMotionLineBuffer offset:0 atIndex:0];
    [motionCompute setBuffer:frameResource->layout_params_buffer offset:0 atIndex:1];
    [motionCompute setTexture:destinationTexture atIndex:0];
    const NSUInteger motionThreadWidth = _pipelines.overlay_legacy_motion.threadExecutionWidth;
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
    [maskCompute setComputePipelineState:_pipelines.overlay_legacy_line_mask];
    [maskCompute setBuffer:_overlayLineRectBuffer offset:0 atIndex:0];
    [maskCompute setBuffer:_overlayLineMaskBuffer offset:0 atIndex:1];
    [maskCompute setBuffer:frameResource->layout_params_buffer offset:0 atIndex:2];
    const NSUInteger maskThreadWidth = _pipelines.overlay_legacy_line_mask.threadExecutionWidth;
    const MTLSize maskThreadsPerThreadgroup = MTLSizeMake(maskThreadWidth, 1, 1);
    const MTLSize maskThreads = MTLSizeMake(overlay->line_rect_count, 1, 1);
    [maskCompute dispatchThreads:maskThreads threadsPerThreadgroup:maskThreadsPerThreadgroup];
    [maskCompute endEncoding];

    id<MTLComputeCommandEncoder> contrastCompute = [commandBuffer computeCommandEncoder];
    if (!contrastCompute) {
      return metal_upload_failure(
          error, errorSize, "failed to create native Metal overlay contrast command");
    }
    [contrastCompute setComputePipelineState:_pipelines.overlay_legacy_line_contrast];
    [contrastCompute setBuffer:_overlayLineMaskBuffer offset:0 atIndex:0];
    [contrastCompute setBuffer:frameResource->layout_params_buffer offset:0 atIndex:1];
    [contrastCompute setTexture:destinationTexture atIndex:0];
    const NSUInteger contrastThreadWidth = _pipelines.overlay_legacy_line_contrast.threadExecutionWidth;
    const NSUInteger contrastThreadHeight =
        std::max<NSUInteger>(1,
                             _pipelines.overlay_legacy_line_contrast.maxTotalThreadsPerThreadgroup /
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
      0,
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
  VPMacOSMetalFrameResources* frameResource =
      [self acquireFrameResourcesWithStagingLength:0 error:error errorSize:errorSize];
  if (!frameResource) {
    return -2;
  }
  bool expected = false;
  if (!_overlayLayerResourcesInFlight.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
    release_resource_flag(&frameResource->in_flight);
    write_error(error, errorSize, "native Metal uploader overlay layer resources are busy");
    return -2;
  }
  ScopedMetalResourceLease reservation(
      {&frameResource->in_flight, &_overlayLayerResourcesInFlight});

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
                                             frameResource:frameResource
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
