#ifndef VOIDPLAYER_MACOS_NATIVE_PLAYER_BRIDGE_H_
#define VOIDPLAYER_MACOS_NATIVE_PLAYER_BRIDGE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPMacOSNativePlayer VPMacOSNativePlayer;
typedef struct VPMacOSMetalUploader VPMacOSMetalUploader;
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

enum {
  VPMacOSMetalUploaderStatusOk = 0,
  VPMacOSMetalUploaderStatusUnavailable = 1,
  VPMacOSMetalUploaderStatusInvalidArguments = 2,
  VPMacOSMetalUploaderStatusSizeMismatch = 3,
  VPMacOSMetalUploaderStatusUnsupportedPixelFormat = 4,
  VPMacOSMetalUploaderStatusTextureWrapFailed = 5,
};

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
void VPMacOSNativeFrameFree(VPMacOSNativeFrame* frame);

VPMacOSMetalUploader* VPMacOSMetalUploaderCreate(void);
void VPMacOSMetalUploaderDestroy(VPMacOSMetalUploader* uploader);
int VPMacOSMetalUploaderIsAvailable(VPMacOSMetalUploader* uploader);
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
    int32_t wait_timeout_ms,
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
