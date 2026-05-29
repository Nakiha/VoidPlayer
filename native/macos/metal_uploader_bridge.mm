#include "macos/metal_uploader_internal.h"

#include <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

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

}  // namespace

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
  return VPMacOSMetalUploaderStatusMessageForCode(status);
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
  write_error(error, error_size, VPMacOSMetalUploaderStatusMessageForCode(status));
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

int VPMacOSMetalUploaderCopyPresentFramePackageWithLayoutAndOverlay(
    VPMacOSMetalUploader* uploader,
    const uint8_t* data,
    size_t data_size,
    const VPMacOSNativePresentFramePackageInfo* package,
    const VPMacOSNativeOverlayGpuPrimitiveSet* overlay,
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
                                         overlay:overlay
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

int VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayoutAndOverlay(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeCVPixelBufferPresentFrame* frame,
    const VPMacOSNativeOverlayGpuPrimitiveSet* overlay,
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
                                              overlay:overlay
                                        toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                                width:width
                                               height:height
                                                  out:out
                                                error:error
                                            errorSize:error_size];
}

int VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameSetWithLayout(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeCVPixelBufferPresentFrameSet* frame_set,
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
  return [uploader->impl copyCVPixelBufferPresentFrameSet:frame_set
                                            toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                                    width:width
                                                   height:height
                                                      out:out
                                                    error:error
                                                errorSize:error_size];
}

int VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameSetWithLayoutAndOverlay(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeCVPixelBufferPresentFrameSet* frame_set,
    const VPMacOSNativeOverlayGpuPrimitiveSet* overlay,
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
  return [uploader->impl copyCVPixelBufferPresentFrameSet:frame_set
                                                 overlay:overlay
                                           toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                                   width:width
                                                  height:height
                                                     out:out
                                                   error:error
                                               errorSize:error_size];
}

int VPMacOSMetalUploaderCompositeOverlayGpuRects(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeOverlayGpuRect* rects,
    size_t rect_count,
    const VPMacOSNativePresentDecisionInfo* decision,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    char* error,
    size_t error_size) {
  if (!uploader || !uploader->impl) {
    write_error(error, error_size, "native Metal uploader is null");
    return -1;
  }
  return [uploader->impl compositeOverlayGpuRects:rects
                                            count:rect_count
                                         decision:decision
                                    toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                            width:width
                                           height:height
                                            error:error
                                        errorSize:error_size];
}

int VPMacOSMetalUploaderCompositeOverlayGpuPrimitives(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeOverlayGpuRect* fill_rects,
    size_t fill_rect_count,
    const VPMacOSNativeOverlayGpuRect* line_rects,
    size_t line_rect_count,
    const VPMacOSNativeOverlayGpuRect* motion_lines,
    size_t motion_line_count,
    const VPMacOSNativePresentDecisionInfo* decision,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    char* error,
    size_t error_size) {
  if (!uploader || !uploader->impl) {
    write_error(error, error_size, "native Metal uploader is null");
    return -1;
  }
  return [uploader->impl compositeOverlayGpuPrimitives:fill_rects
                                             fillCount:fill_rect_count
                                             lineRects:line_rects
                                             lineCount:line_rect_count
                                           motionLines:motion_lines
                                           motionCount:motion_line_count
                                              decision:decision
                                         toPixelBuffer:(CVPixelBufferRef)pixel_buffer
                                                 width:width
                                                height:height
                                                 error:error
                                             errorSize:error_size];
}
