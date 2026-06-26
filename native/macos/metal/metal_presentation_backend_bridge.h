#ifndef VOIDPLAYER_MACOS_METAL_PRESENTATION_BACKEND_BRIDGE_H_
#define VOIDPLAYER_MACOS_METAL_PRESENTATION_BACKEND_BRIDGE_H_

#include "metal_uploader_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPMacOSMetalPresentationBackend VPMacOSMetalPresentationBackend;

VPMacOSMetalPresentationBackend* VPMacOSMetalPresentationBackendCreate(
    int32_t width,
    int32_t height);
void VPMacOSMetalPresentationBackendDestroy(VPMacOSMetalPresentationBackend* backend);
int VPMacOSMetalPresentationBackendIsAvailable(VPMacOSMetalPresentationBackend* backend);
VPMacOSMetalUploader* VPMacOSMetalPresentationBackendUploader(
    VPMacOSMetalPresentationBackend* backend);
void VPMacOSMetalPresentationBackendSetDrawTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots);
void VPMacOSMetalPresentationBackendSetDrawTargetRing(
    VPMacOSMetalPresentationBackend* backend,
    const void* const* pixel_buffers,
    size_t pixel_buffer_count,
    void* displayed_pixel_buffer,
    void* protected_pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots);
void VPMacOSMetalPresentationBackendClearDrawTarget(
    VPMacOSMetalPresentationBackend* backend);
int VPMacOSMetalPresentationBackendContainsDrawTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer);
void VPMacOSMetalPresentationBackendMarkDisplayedTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer);
void VPMacOSMetalPresentationBackendProtectTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer);
void VPMacOSMetalPresentationBackendReleaseTarget(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer);
int64_t VPMacOSMetalPresentationBackendDirectYUVUploadCount(
    VPMacOSMetalPresentationBackend* backend);
int64_t VPMacOSMetalPresentationBackendCVPixelBufferUploadCount(
    VPMacOSMetalPresentationBackend* backend);
int64_t VPMacOSMetalPresentationBackendPresentPackageUploadCount(
    VPMacOSMetalPresentationBackend* backend);
int64_t VPMacOSMetalPresentationBackendLastPresentPackageCopyUs(
    VPMacOSMetalPresentationBackend* backend);
int64_t VPMacOSMetalPresentationBackendLastPresentPackageGpuWaitUs(
    VPMacOSMetalPresentationBackend* backend);
int64_t VPMacOSMetalPresentationBackendLastPresentPackageTotalUs(
    VPMacOSMetalPresentationBackend* backend);
int32_t VPMacOSMetalPresentationBackendLastPresentPackageStorage(
    VPMacOSMetalPresentationBackend* backend);
int VPMacOSMetalPresentationBackendValidatePixelBufferChecked(
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    char* error,
    size_t error_size);
int VPMacOSMetalPresentationBackendCopyPresentFramePackageWithLayout(
    VPMacOSMetalPresentationBackend* backend,
    const uint8_t* data,
    size_t data_size,
    const VPMacOSNativePresentFramePackageInfo* package,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);
int VPMacOSMetalPresentationBackendCopyCVPixelBufferPresentFrameWithLayout(
    VPMacOSMetalPresentationBackend* backend,
    const VPMacOSNativeCVPixelBufferPresentFrame* frame,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);

#ifdef __cplusplus
}  // extern "C"
#endif

#ifdef __cplusplus
namespace vr {
class PresentationBackend;
}  // namespace vr

vr::PresentationBackend* VPMacOSMetalPresentationBackendSourceBakeBackend(
    VPMacOSMetalPresentationBackend* backend,
    void* initial_pixel_buffer,
    int32_t width,
    int32_t height,
    char* error,
    size_t error_size);
#endif

#endif  // VOIDPLAYER_MACOS_METAL_PRESENTATION_BACKEND_BRIDGE_H_
