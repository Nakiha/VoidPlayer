#ifndef VOIDPLAYER_MACOS_METAL_UPLOADER_BRIDGE_H_
#define VOIDPLAYER_MACOS_METAL_UPLOADER_BRIDGE_H_

#include "native_player_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPMacOSMetalUploader VPMacOSMetalUploader;
typedef void (*VPMacOSMetalUploaderCompletion)(
    void* user_data,
    int ret,
    VPMacOSNativeFrameInfo frame_info,
    const char* error,
    int64_t gpu_wait_us,
    int64_t total_us);

enum {
  VPMacOSMetalUploaderStatusOk = 0,
  VPMacOSMetalUploaderStatusUnavailable = 1,
  VPMacOSMetalUploaderStatusInvalidArguments = 2,
  VPMacOSMetalUploaderStatusSizeMismatch = 3,
  VPMacOSMetalUploaderStatusUnsupportedPixelFormat = 4,
  VPMacOSMetalUploaderStatusTextureWrapFailed = 5,
  VPMacOSNativePresentFormatBGRA = 0,
  VPMacOSNativePresentFormatNV12 = 1,
  VPMacOSNativePresentFormatP010 = 2,
  VPMacOSNativePresentFormatYUV420P = 3,
  VPMacOSNativePresentPackageStorageUnavailable = 0,
  VPMacOSNativePresentPackageStorageYUV = 1,
  VPMacOSNativePresentPackageStorageBGRA = 2,
  VPMacOSNativePresentPackageStorageCVPixelBuffer = 3,
};

typedef struct VPMacOSNativePresentFrameInfo {
  int32_t present;
  int32_t file_id;
  int32_t slot;
  int32_t width;
  int32_t height;
  int64_t pts_us;
  int64_t dts_us;
  int64_t duration_us;
  int32_t analysis_frame_index;
  int32_t frame_identity_mode;
  int32_t source_packet_index;
  int32_t source_packet_size;
  int64_t source_packet_pos;
  int64_t source_packet_pts;
  int64_t source_packet_dts;
} VPMacOSNativePresentFrameInfo;

typedef struct VPMacOSNativePresentDecisionInfo {
  int32_t should_present;
  int32_t frame_count;
  int32_t track_count;
  int32_t mode;
  int64_t current_pts_us;
  float split_pos;
  int32_t order[VPMacOSNativeMaxTracks];
  float display_offset_x[VPMacOSNativeMaxTracks];
  float display_offset_y[VPMacOSNativeMaxTracks];
  float inv_display_size_x[VPMacOSNativeMaxTracks];
  float inv_display_size_y[VPMacOSNativeMaxTracks];
  float view_offset_uv_x[VPMacOSNativeMaxTracks];
  float view_offset_uv_y[VPMacOSNativeMaxTracks];
  int32_t source_width[VPMacOSNativeMaxTracks];
  int32_t source_height[VPMacOSNativeMaxTracks];
  int32_t yuv_format[VPMacOSNativeMaxTracks];
  int32_t y_offset[VPMacOSNativeMaxTracks];
  int32_t uv_offset[VPMacOSNativeMaxTracks];
  int32_t v_offset[VPMacOSNativeMaxTracks];
  int32_t y_stride[VPMacOSNativeMaxTracks];
  int32_t uv_stride[VPMacOSNativeMaxTracks];
  int32_t coded_width[VPMacOSNativeMaxTracks];
  int32_t coded_height[VPMacOSNativeMaxTracks];
  float nv12_uv_scale_x[VPMacOSNativeMaxTracks];
  float nv12_uv_scale_y[VPMacOSNativeMaxTracks];
  int32_t color_range[VPMacOSNativeMaxTracks];
  int32_t color_matrix[VPMacOSNativeMaxTracks];
  int32_t color_transfer[VPMacOSNativeMaxTracks];
  int32_t color_primaries[VPMacOSNativeMaxTracks];
  VPMacOSNativePresentFrameInfo frames[VPMacOSNativeMaxTracks];
} VPMacOSNativePresentDecisionInfo;

typedef struct VPMacOSNativePresentFramePackageInfo {
  int32_t storage;
  int32_t width;
  int32_t height;
  int32_t max_track_slots;
  int32_t stride_bytes;
  size_t track_stride_bytes;
  size_t used_bytes;
  VPMacOSNativePresentDecisionInfo decision;
} VPMacOSNativePresentFramePackageInfo;

typedef struct VPMacOSNativeCVPixelBufferPresentFrame {
  void* pixel_buffer;
  int32_t pixel_format;
  int32_t plane_count;
  int32_t is_p010;
  int32_t coded_width;
  int32_t coded_height;
  VPMacOSNativePresentDecisionInfo decision;
} VPMacOSNativeCVPixelBufferPresentFrame;

typedef struct VPMacOSNativeCVPixelBufferPresentFrameSet {
  void* pixel_buffers[VPMacOSNativeMaxTracks];
  int32_t pixel_formats[VPMacOSNativeMaxTracks];
  int32_t plane_counts[VPMacOSNativeMaxTracks];
  int32_t is_p010[VPMacOSNativeMaxTracks];
  int32_t coded_widths[VPMacOSNativeMaxTracks];
  int32_t coded_heights[VPMacOSNativeMaxTracks];
  VPMacOSNativePresentDecisionInfo decision;
} VPMacOSNativeCVPixelBufferPresentFrameSet;

typedef struct VPMacOSNativeOverlayGpuRect {
  uint32_t rect_uv0;
  uint32_t rect_uv1;
  uint32_t color_bgra;
  uint32_t track_idx;
} VPMacOSNativeOverlayGpuRect;

typedef struct VPMacOSNativeOverlayGpuPrimitiveSet {
  const VPMacOSNativeOverlayGpuRect* fill_rects;
  size_t fill_rect_count;
  const VPMacOSNativeOverlayGpuRect* line_rects;
  size_t line_rect_count;
  const VPMacOSNativeOverlayGpuRect* motion_lines;
  size_t motion_line_count;
  uint64_t generation;
} VPMacOSNativeOverlayGpuPrimitiveSet;

VPMacOSMetalUploader* VPMacOSMetalUploaderCreate(void);
void VPMacOSMetalUploaderDestroy(VPMacOSMetalUploader* uploader);
int VPMacOSMetalUploaderIsAvailable(VPMacOSMetalUploader* uploader);
int64_t VPMacOSMetalUploaderDirectYUVUploadCount(VPMacOSMetalUploader* uploader);
int64_t VPMacOSMetalUploaderCVPixelBufferUploadCount(VPMacOSMetalUploader* uploader);
int64_t VPMacOSMetalUploaderPresentPackageUploadCount(VPMacOSMetalUploader* uploader);
int64_t VPMacOSMetalUploaderLastPresentPackageCopyUs(VPMacOSMetalUploader* uploader);
int64_t VPMacOSMetalUploaderLastPresentPackageGpuWaitUs(VPMacOSMetalUploader* uploader);
int64_t VPMacOSMetalUploaderLastPresentPackageTotalUs(VPMacOSMetalUploader* uploader);
int32_t VPMacOSMetalUploaderLastPresentPackageStorage(VPMacOSMetalUploader* uploader);
int VPMacOSMetalUploaderValidatePixelBuffer(VPMacOSMetalUploader* uploader,
                                            void* pixel_buffer,
                                            int32_t width,
                                            int32_t height);
const char* VPMacOSMetalUploaderStatusMessage(int status);
int VPMacOSMetalUploaderValidatePixelBufferChecked(VPMacOSMetalUploader* uploader,
                                                   void* pixel_buffer,
                                                   int32_t width,
                                                   int32_t height,
                                                   char* error,
                                                   size_t error_size);
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
    size_t error_size);
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
    size_t error_size);
int VPMacOSMetalUploaderCopyPresentFramePackageWithLayoutAndOverlayAsync(
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
    size_t error_size,
    VPMacOSMetalUploaderCompletion completion,
    void* user_data);
// Deprecated ABI placeholders. Prepared package upload depended on mutable
// uploader-internal staging bytes and is disabled; use
// VPMacOSMetalUploaderCopyPresentFramePackageWithLayout* instead.
int VPMacOSMetalUploaderUploadPreparedPresentFramePackageWithLayoutAndOverlay(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativePresentFramePackageInfo* package,
    const VPMacOSNativeOverlayGpuPrimitiveSet* overlay,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);
int VPMacOSMetalUploaderUploadPreparedPresentFramePackageWithLayoutAndOverlayAsync(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativePresentFramePackageInfo* package,
    const VPMacOSNativeOverlayGpuPrimitiveSet* overlay,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size,
    VPMacOSMetalUploaderCompletion completion,
    void* user_data);
int VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayout(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeCVPixelBufferPresentFrame* frame,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);
int VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayoutAndOverlay(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeCVPixelBufferPresentFrame* frame,
    const VPMacOSNativeOverlayGpuPrimitiveSet* overlay,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);
int VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameWithLayoutAndOverlayAsync(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeCVPixelBufferPresentFrame* frame,
    const VPMacOSNativeOverlayGpuPrimitiveSet* overlay,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size,
    VPMacOSMetalUploaderCompletion completion,
    void* user_data);
int VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameSetWithLayout(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeCVPixelBufferPresentFrameSet* frame_set,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);
int VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameSetWithLayoutAndOverlay(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeCVPixelBufferPresentFrameSet* frame_set,
    const VPMacOSNativeOverlayGpuPrimitiveSet* overlay,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);
int VPMacOSMetalUploaderCopyCVPixelBufferPresentFrameSetWithLayoutAndOverlayAsync(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeCVPixelBufferPresentFrameSet* frame_set,
    const VPMacOSNativeOverlayGpuPrimitiveSet* overlay,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size,
    VPMacOSMetalUploaderCompletion completion,
    void* user_data);
int VPMacOSMetalUploaderCompositeOverlayGpuRects(
    VPMacOSMetalUploader* uploader,
    const VPMacOSNativeOverlayGpuRect* rects,
    size_t rect_count,
    const VPMacOSNativePresentDecisionInfo* decision,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    char* error,
    size_t error_size);
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
    size_t error_size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VOIDPLAYER_MACOS_METAL_UPLOADER_BRIDGE_H_
