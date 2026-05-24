#ifndef VOIDPLAYER_MACOS_NATIVE_PLAYER_BRIDGE_H_
#define VOIDPLAYER_MACOS_NATIVE_PLAYER_BRIDGE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPMacOSNativePlayer VPMacOSNativePlayer;
typedef struct VPMacOSMetalUploader VPMacOSMetalUploader;
typedef struct VPMacOSMetalPresentationBackend VPMacOSMetalPresentationBackend;
typedef void (*VPMacOSFrameAvailableCallback)(void* user_data);

typedef struct VPMacOSNativeFrame {
  int32_t width;
  int32_t height;
  int64_t pts_us;
  int64_t dts_us;
  int64_t duration_us;
  uint8_t* bgra;
  size_t bgra_size;
} VPMacOSNativeFrame;

typedef struct VPMacOSNativeFrameInfo {
  int32_t width;
  int32_t height;
  int64_t pts_us;
  int64_t dts_us;
  int64_t duration_us;
} VPMacOSNativeFrameInfo;

typedef struct VPMacOSNativeTrackInfo {
  int32_t file_id;
  int32_t slot;
  int32_t width;
  int32_t height;
  int64_t duration_us;
} VPMacOSNativeTrackInfo;

typedef struct VPMacOSCaptureMetrics {
  int32_t width;
  int32_t height;
  double avg_luma;
  double non_black_ratio;
  uint64_t hash;
} VPMacOSCaptureMetrics;

typedef struct VPMacOSNativeLayoutState {
  int32_t mode;
  float split_pos;
  float zoom_ratio;
  float view_offset_x;
  float view_offset_y;
  int32_t pixel_size_mode;
  int32_t order[4];
} VPMacOSNativeLayoutState;

typedef struct VPMacOSNativeLayoutPresentationParams {
  float display_offset_x;
  float display_offset_y;
  float inv_display_size_x;
  float inv_display_size_y;
  float view_offset_uv_x;
  float view_offset_uv_y;
} VPMacOSNativeLayoutPresentationParams;

typedef struct VPMacOSNativePresentationSchedulerStats {
  uint64_t tick_count;
  uint64_t presentable_tick_count;
  uint64_t frame_notification_count;
  int64_t last_selected_pts_us;
  int32_t last_present_frame_count;
  int32_t cached_present_decision_available;
  uint64_t deadline_sleep_count;
  int64_t last_deadline_sleep_us;
} VPMacOSNativePresentationSchedulerStats;

enum {
  VPMacOSNativeMaxTracks = 4,
  VPMacOSMetalUploaderStatusOk = 0,
  VPMacOSMetalUploaderStatusUnavailable = 1,
  VPMacOSMetalUploaderStatusInvalidArguments = 2,
  VPMacOSMetalUploaderStatusSizeMismatch = 3,
  VPMacOSMetalUploaderStatusUnsupportedPixelFormat = 4,
  VPMacOSMetalUploaderStatusTextureWrapFailed = 5,
  VPMacOSNativePresentFormatBGRA = 0,
  VPMacOSNativePresentFormatNV12 = 1,
  VPMacOSNativePresentFormatP010 = 2,
  VPMacOSNativePresentPackageStorageUnavailable = 0,
  VPMacOSNativePresentPackageStorageYUV = 1,
  VPMacOSNativePresentPackageStorageBGRA = 2,
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

VPMacOSNativePlayer* VPMacOSNativePlayerCreate(void);
void VPMacOSNativePlayerDestroy(VPMacOSNativePlayer* player);

int VPMacOSNativePlayerOpen(VPMacOSNativePlayer* player,
                            const char* path,
                            char* error,
                            size_t error_size);
int VPMacOSNativePlayerAddTrack(VPMacOSNativePlayer* player,
                                const char* path,
                                int32_t file_id,
                                VPMacOSNativeTrackInfo* out,
                                char* error,
                                size_t error_size);
void VPMacOSNativePlayerRemoveTrack(VPMacOSNativePlayer* player,
                                    int32_t file_id);
void VPMacOSNativePlayerClose(VPMacOSNativePlayer* player);
void VPMacOSNativePlayerSetFrameAvailableCallback(
    VPMacOSNativePlayer* player,
    VPMacOSFrameAvailableCallback callback,
    void* user_data);
int VPMacOSNativePlayerSetMetalPresentationTarget(
    VPMacOSNativePlayer* player,
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots);
void VPMacOSNativePlayerClearMetalPresentationTarget(VPMacOSNativePlayer* player);
int VPMacOSNativePlayerRendererOwnedPresentationActive(VPMacOSNativePlayer* player);
int VPMacOSNativePlayerLastRendererOwnedPresentationSucceeded(VPMacOSNativePlayer* player);

void VPMacOSNativePlayerPlay(VPMacOSNativePlayer* player);
void VPMacOSNativePlayerPause(VPMacOSNativePlayer* player);
void VPMacOSNativePlayerSetSpeed(VPMacOSNativePlayer* player, double speed);
void VPMacOSNativePlayerSetLoopRange(VPMacOSNativePlayer* player,
                                     int enabled,
                                     int64_t start_us,
                                     int64_t end_us);
void VPMacOSNativePlayerSetAudibleTrack(VPMacOSNativePlayer* player,
                                        int32_t file_id);
void VPMacOSNativePlayerSetTrackOffset(VPMacOSNativePlayer* player,
                                       int32_t file_id,
                                       int64_t offset_us);
int64_t VPMacOSNativePlayerTrackOffsetUs(VPMacOSNativePlayer* player,
                                         int32_t file_id);
void VPMacOSNativePlayerApplyLayout(VPMacOSNativePlayer* player,
                                    const VPMacOSNativeLayoutState* state);
int VPMacOSNativePlayerCopyLayout(VPMacOSNativePlayer* player,
                                  VPMacOSNativeLayoutState* out);
int VPMacOSNativePlayerCopyLayoutPresentationParams(
    VPMacOSNativePlayer* player,
    int32_t width,
    int32_t height,
    VPMacOSNativeLayoutPresentationParams* out);
void VPMacOSNativePlayerSeek(VPMacOSNativePlayer* player, int64_t pts_us);

int64_t VPMacOSNativePlayerCurrentPtsUs(VPMacOSNativePlayer* player);
int64_t VPMacOSNativePlayerDurationUs(VPMacOSNativePlayer* player);
int32_t VPMacOSNativePlayerWidth(VPMacOSNativePlayer* player);
int32_t VPMacOSNativePlayerHeight(VPMacOSNativePlayer* player);
int VPMacOSNativePlayerIsPlaying(VPMacOSNativePlayer* player);
int VPMacOSNativePlayerHasAudio(VPMacOSNativePlayer* player);
int32_t VPMacOSNativePlayerAudioSampleRate(VPMacOSNativePlayer* player);
int32_t VPMacOSNativePlayerAudioChannels(VPMacOSNativePlayer* player);
int32_t VPMacOSNativePlayerActiveAudioTrack(VPMacOSNativePlayer* player);
int VPMacOSNativePlayerHardwareDecodeActive(VPMacOSNativePlayer* player);
int VPMacOSNativePlayerHardwareDecodeDownloadsToCpu(VPMacOSNativePlayer* player);
const char* VPMacOSNativePlayerDecodeModeName(VPMacOSNativePlayer* player);
const char* VPMacOSNativePlayerDecoderName(VPMacOSNativePlayer* player);
const char* VPMacOSNativePresentationAdapterName(void);
const char* VPMacOSNativePresentationSchedulerName(void);
int VPMacOSNativePlayerCopyPresentationSchedulerStats(
    VPMacOSNativePlayer* player,
    VPMacOSNativePresentationSchedulerStats* out);
int VPMacOSNativeHardwareDecodeAvailable(void);
const char* VPMacOSNativeHardwareDecodeProviderName(void);

int VPMacOSNativePlayerCopyCurrentFrameBGRA(VPMacOSNativePlayer* player,
                                            VPMacOSNativeFrame* out,
                                            char* error,
                                            size_t error_size);
int VPMacOSNativePlayerCopyCurrentFrameBGRAInto(VPMacOSNativePlayer* player,
                                                uint8_t* dst,
                                                size_t dst_size,
                                                int32_t width,
                                                int32_t height,
                                                int32_t stride_bytes,
                                                VPMacOSNativeFrameInfo* out,
                                                char* error,
                                                size_t error_size);
int VPMacOSNativePlayerCopyPresentFramesBGRAInto(
    VPMacOSNativePlayer* player,
    uint8_t* dst,
    size_t dst_size,
    int32_t width,
    int32_t height,
    int32_t stride_bytes,
    size_t track_stride_bytes,
    VPMacOSNativePresentDecisionInfo* out,
    char* error,
    size_t error_size);
int VPMacOSNativePlayerCopyPresentFramesYUVInto(
    VPMacOSNativePlayer* player,
    uint8_t* dst,
    size_t dst_size,
    int32_t width,
    int32_t height,
    size_t max_track_slots,
    VPMacOSNativePresentDecisionInfo* out,
    char* error,
    size_t error_size);
size_t VPMacOSNativePresentFramePackageMaxBytes(int32_t width,
                                                int32_t height,
                                                int32_t max_track_slots);
int VPMacOSNativePlayerCopyPresentFramePackage(
    VPMacOSNativePlayer* player,
    uint8_t* dst,
    size_t dst_size,
    int32_t width,
    int32_t height,
    int32_t max_track_slots,
    VPMacOSNativePresentFramePackageInfo* out,
    char* error,
    size_t error_size);
void VPMacOSNativeFrameFree(VPMacOSNativeFrame* frame);

VPMacOSMetalUploader* VPMacOSMetalUploaderCreate(void);
void VPMacOSMetalUploaderDestroy(VPMacOSMetalUploader* uploader);
int VPMacOSMetalUploaderIsAvailable(VPMacOSMetalUploader* uploader);
int64_t VPMacOSMetalUploaderDirectYUVUploadCount(VPMacOSMetalUploader* uploader);
int64_t VPMacOSMetalUploaderPresentPackageUploadCount(VPMacOSMetalUploader* uploader);
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
int VPMacOSMetalUploaderCopyCurrentFrame(VPMacOSMetalUploader* uploader,
                                         VPMacOSNativePlayer* player,
                                         void* pixel_buffer,
                                         int32_t width,
                                         int32_t height,
                                         int32_t wait_timeout_ms,
                                         VPMacOSNativeFrameInfo* out,
                                         char* error,
                                         size_t error_size);
int VPMacOSMetalUploaderCopyCurrentFrameWithLayout(
    VPMacOSMetalUploader* uploader,
    VPMacOSNativePlayer* player,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots,
    int32_t wait_timeout_ms,
    VPMacOSNativeFrameInfo* out,
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

VPMacOSMetalPresentationBackend* VPMacOSMetalPresentationBackendCreate(
    int32_t width,
    int32_t height);
void VPMacOSMetalPresentationBackendDestroy(VPMacOSMetalPresentationBackend* backend);
int VPMacOSMetalPresentationBackendIsAvailable(VPMacOSMetalPresentationBackend* backend);
VPMacOSMetalUploader* VPMacOSMetalPresentationBackendUploader(
    VPMacOSMetalPresentationBackend* backend);
int64_t VPMacOSMetalPresentationBackendDirectYUVUploadCount(
    VPMacOSMetalPresentationBackend* backend);
int64_t VPMacOSMetalPresentationBackendPresentPackageUploadCount(
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
int VPMacOSMetalPresentationBackendCopyCurrentFrameWithLayout(
    VPMacOSMetalPresentationBackend* backend,
    VPMacOSNativePlayer* player,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots,
    int32_t wait_timeout_ms,
    VPMacOSNativeFrameInfo* out,
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

int VPMacOSMeasureBGRA(const uint8_t* bgra,
                       int32_t width,
                       int32_t height,
                       int32_t stride_bytes,
                       VPMacOSCaptureMetrics* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VOIDPLAYER_MACOS_NATIVE_PLAYER_BRIDGE_H_
