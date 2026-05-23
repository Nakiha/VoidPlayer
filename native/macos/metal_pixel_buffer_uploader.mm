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

constant int kModeSplitScreen = 1;
constant uint kMaxTracks = 4;

struct LayoutParams {
  uint width;
  uint height;
  int mode;
  int track_count;
  float split_pos;
  uint frame_present0;
  uint frame_present1;
  uint frame_present2;
  uint frame_present3;
  int order0;
  int order1;
  int order2;
  int order3;
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

uint frame_present_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.frame_present0;
  if (index == 1) return params.frame_present1;
  if (index == 2) return params.frame_present2;
  return params.frame_present3;
}

int order_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.order0;
  if (index == 1) return params.order1;
  if (index == 2) return params.order2;
  return params.order3;
}

float display_offset_x_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.display_offset_x0;
  if (index == 1) return params.display_offset_x1;
  if (index == 2) return params.display_offset_x2;
  return params.display_offset_x3;
}

float display_offset_y_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.display_offset_y0;
  if (index == 1) return params.display_offset_y1;
  if (index == 2) return params.display_offset_y2;
  return params.display_offset_y3;
}

float inv_display_size_x_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.inv_display_size_x0;
  if (index == 1) return params.inv_display_size_x1;
  if (index == 2) return params.inv_display_size_x2;
  return params.inv_display_size_x3;
}

float inv_display_size_y_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.inv_display_size_y0;
  if (index == 1) return params.inv_display_size_y1;
  if (index == 2) return params.inv_display_size_y2;
  return params.inv_display_size_y3;
}

float view_offset_uv_x_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.view_offset_uv_x0;
  if (index == 1) return params.view_offset_uv_x1;
  if (index == 2) return params.view_offset_uv_x2;
  return params.view_offset_uv_x3;
}

float view_offset_uv_y_at(constant LayoutParams& params, uint index) {
  if (index == 0) return params.view_offset_uv_y0;
  if (index == 1) return params.view_offset_uv_y1;
  if (index == 2) return params.view_offset_uv_y2;
  return params.view_offset_uv_y3;
}

float2 aspect_fit_uv(float2 local_uv,
                     constant LayoutParams& params,
                     uint track_idx,
                     thread bool& out_of_bounds) {
  const float2 display_offset = float2(
      display_offset_x_at(params, track_idx),
      display_offset_y_at(params, track_idx));
  const float2 inv_display_size = float2(
      inv_display_size_x_at(params, track_idx),
      inv_display_size_y_at(params, track_idx));
  const float2 view_offset_uv = float2(
      view_offset_uv_x_at(params, track_idx),
      view_offset_uv_y_at(params, track_idx));
  const float2 source_uv = (local_uv - display_offset) * inv_display_size - view_offset_uv;
  if (source_uv.x < 0.0 || source_uv.x > 1.0 ||
      source_uv.y < 0.0 || source_uv.y > 1.0) {
    out_of_bounds = true;
    return float2(0.0, 0.0);
  }
  out_of_bounds = false;
  return source_uv;
}

kernel void layout_bgra_copy(
    device const uchar4* source [[buffer(0)]],
    constant LayoutParams& params [[buffer(1)]],
    texture2d<float, access::write> destination [[texture(0)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= params.width || gid.y >= params.height) {
    return;
  }

  const float2 canvas_size = float2(float(params.width), float(params.height));
  const float2 texcoord = (float2(gid) + float2(0.5, 0.5)) / canvas_size;
  int track_idx = 0;
  float2 local_uv = texcoord;
  if (params.mode == kModeSplitScreen) {
    track_idx = texcoord.x < params.split_pos
        ? order_at(params, 0)
        : order_at(params, 1);
  } else {
    const int count = max(params.track_count, 1);
    const float scaled_x = texcoord.x * float(count);
    const int display_slot = clamp(int(scaled_x), 0, count - 1);
    track_idx = order_at(params, uint(display_slot));
    local_uv = float2(scaled_x - float(display_slot), texcoord.y);
  }
  track_idx = clamp(track_idx, 0, int(kMaxTracks) - 1);
  const uint track_slot = uint(track_idx);
  if (frame_present_at(params, track_slot) == 0) {
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    return;
  }

  bool out_of_bounds = false;
  const float2 source_uv = aspect_fit_uv(local_uv, params, track_slot, out_of_bounds);
  if (out_of_bounds) {
    destination.write(float4(0.0, 0.0, 0.0, 1.0), gid);
    return;
  }

  const uint source_x = min(uint(source_uv.x * float(params.width)), params.width - 1);
  const uint source_y = min(uint(source_uv.y * float(params.height)), params.height - 1);
  const uint track_offset = track_slot * params.width * params.height;
  const uchar4 bgra = source[track_offset + source_y * params.width + source_x];
  float4 color =
      float4(float(bgra.z), float(bgra.y), float(bgra.x), float(bgra.w)) / 255.0;
  if (params.mode == kModeSplitScreen && params.width > 0) {
    const float divider_x = params.split_pos * float(params.width);
    const float pixel_x = texcoord.x * float(params.width);
    const float dist = abs(pixel_x - divider_x);
    const float core_width = 1.25;
    const float edge_width = 0.75;
    if (dist <= core_width + edge_width) {
      const float alpha = (dist <= core_width)
          ? 1.0
          : 1.0 - ((dist - core_width) / edge_width);
      const float3 divider_color = 1.0 - color.rgb;
      color.rgb = divider_color * alpha + color.rgb * (1.0 - alpha);
      color.a = 1.0;
    }
  }
  destination.write(color, gid);
}
)";

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
  size_t stagingSize = 0;
  if (!checked_mul_size(static_cast<size_t>(width), 4u, &rowBytes) ||
      !checked_mul_size(rowBytes, static_cast<size_t>(height), &uploadSize) ||
      !checked_mul_size(uploadSize, static_cast<size_t>(VPMacOSNativeMaxTracks), &stagingSize)) {
    write_error(error, errorSize, "native Metal layout upload dimensions overflow");
    return -1;
  }
  if (rowBytes > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    write_error(error, errorSize, "native Metal layout upload row stride is too large");
    return -1;
  }
  if (![self ensureStagingBufferWithLength:stagingSize] ||
      ![self ensureLayoutParamsBuffer]) {
    return metal_upload_failure(
        error, errorSize, "failed to allocate native Metal layout buffers");
  }
  std::memset([_stagingBuffer contents], 0, stagingSize);

  VPMacOSNativePresentDecisionInfo decisionInfo = {};
  const int copyRet = VPMacOSNativePlayerCopyPresentFramesBGRAInto(
      player,
      static_cast<uint8_t*>([_stagingBuffer contents]),
      stagingSize,
      width,
      height,
      static_cast<int32_t>(rowBytes),
      uploadSize,
      &decisionInfo,
      error,
      errorSize);
  if (copyRet != 0) {
    if (error && std::strcmp(error, "not all present decision frames are ready") == 0) {
      return -1;
    }
    if (!error || error[0] == '\0') {
      write_error(error, errorSize, "failed to copy native present frames");
    }
    return -2;
  }
  if (out) {
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

  auto* metalParams = static_cast<MetalLayoutParams*>([_layoutParamsBuffer contents]);
  metalParams->width = static_cast<uint32_t>(width);
  metalParams->height = static_cast<uint32_t>(height);
  metalParams->mode = decisionInfo.mode;
  metalParams->track_count = decisionInfo.track_count;
  metalParams->split_pos = decisionInfo.split_pos;
  metalParams->frame_present0 =
      static_cast<uint32_t>(decisionInfo.frames[0].present ? 1u : 0u);
  metalParams->frame_present1 =
      static_cast<uint32_t>(decisionInfo.frames[1].present ? 1u : 0u);
  metalParams->frame_present2 =
      static_cast<uint32_t>(decisionInfo.frames[2].present ? 1u : 0u);
  metalParams->frame_present3 =
      static_cast<uint32_t>(decisionInfo.frames[3].present ? 1u : 0u);
  metalParams->order0 = decisionInfo.order[0];
  metalParams->order1 = decisionInfo.order[1];
  metalParams->order2 = decisionInfo.order[2];
  metalParams->order3 = decisionInfo.order[3];
  metalParams->display_offset_x0 = decisionInfo.display_offset_x[0];
  metalParams->display_offset_x1 = decisionInfo.display_offset_x[1];
  metalParams->display_offset_x2 = decisionInfo.display_offset_x[2];
  metalParams->display_offset_x3 = decisionInfo.display_offset_x[3];
  metalParams->display_offset_y0 = decisionInfo.display_offset_y[0];
  metalParams->display_offset_y1 = decisionInfo.display_offset_y[1];
  metalParams->display_offset_y2 = decisionInfo.display_offset_y[2];
  metalParams->display_offset_y3 = decisionInfo.display_offset_y[3];
  metalParams->inv_display_size_x0 = decisionInfo.inv_display_size_x[0];
  metalParams->inv_display_size_x1 = decisionInfo.inv_display_size_x[1];
  metalParams->inv_display_size_x2 = decisionInfo.inv_display_size_x[2];
  metalParams->inv_display_size_x3 = decisionInfo.inv_display_size_x[3];
  metalParams->inv_display_size_y0 = decisionInfo.inv_display_size_y[0];
  metalParams->inv_display_size_y1 = decisionInfo.inv_display_size_y[1];
  metalParams->inv_display_size_y2 = decisionInfo.inv_display_size_y[2];
  metalParams->inv_display_size_y3 = decisionInfo.inv_display_size_y[3];
  metalParams->view_offset_uv_x0 = decisionInfo.view_offset_uv_x[0];
  metalParams->view_offset_uv_x1 = decisionInfo.view_offset_uv_x[1];
  metalParams->view_offset_uv_x2 = decisionInfo.view_offset_uv_x[2];
  metalParams->view_offset_uv_x3 = decisionInfo.view_offset_uv_x[3];
  metalParams->view_offset_uv_y0 = decisionInfo.view_offset_uv_y[0];
  metalParams->view_offset_uv_y1 = decisionInfo.view_offset_uv_y[1];
  metalParams->view_offset_uv_y2 = decisionInfo.view_offset_uv_y[2];
  metalParams->view_offset_uv_y3 = decisionInfo.view_offset_uv_y[3];

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
