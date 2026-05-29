#ifndef VOIDPLAYER_MACOS_NATIVE_PLAYER_BRIDGE_H_
#define VOIDPLAYER_MACOS_NATIVE_PLAYER_BRIDGE_H_

#include "metal_presentation_backend_bridge.h"
#include "native_player_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void VPMacOSInstallCrashHandler(const char* crash_dir);
void VPMacOSRemoveCrashHandler(void);
void VPMacOSConfigureLogging(const char* logs_dir,
                             const char* log_file_name,
                             const char* level);
void VPMacOSLogProfilerSummary(const char* message);

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
int VPMacOSNativePlayerCopyTrackInfo(VPMacOSNativePlayer* player,
                                     int32_t file_id,
                                     VPMacOSNativeTrackInfo* out);
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
int VPMacOSNativePlayerInstallMetalPresentationTarget(
    VPMacOSNativePlayer* player,
    VPMacOSMetalPresentationBackend* backend,
    void* pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots);
void VPMacOSNativePlayerClearMetalPresentationTarget(VPMacOSNativePlayer* player);
int VPMacOSNativePlayerRendererOwnedPresentationActive(VPMacOSNativePlayer* player);
int VPMacOSNativePlayerLastRendererOwnedPresentationSucceeded(VPMacOSNativePlayer* player);
int VPMacOSNativePlayerCopyLastRendererOwnedFrameInfo(
    VPMacOSNativePlayer* player,
    VPMacOSNativeFrameInfo* out);
int VPMacOSNativePlayerCopyRendererOwnedPresentationState(
    VPMacOSNativePlayer* player,
    VPMacOSNativeRendererOwnedPresentationState* out);
int VPMacOSNativePlayerCopyTrackDiagnostics(
    VPMacOSNativePlayer* player,
    VPMacOSNativeTrackDiagnosticInfo* out,
    size_t capacity,
    size_t* out_count);
void VPMacOSNativePlayerResetRendererOwnedPresentationStats(VPMacOSNativePlayer* player);
uint64_t VPMacOSNativePlayerRendererOwnedPresentationUploadCount(
    VPMacOSNativePlayer* player);
uint64_t VPMacOSNativePlayerRendererOwnedPresentationFailureCount(
    VPMacOSNativePlayer* player);
int VPMacOSNativePlayerPresentCurrentFrameToMetalTarget(
    VPMacOSNativePlayer* player,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);
int VPMacOSNativePlayerRequestRendererOwnedFrameRefresh(
    VPMacOSNativePlayer* player,
    int32_t timeout_ms,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);

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
int VPMacOSNativePlayerStepForward(VPMacOSNativePlayer* player,
                                   char* error,
                                   size_t error_size);
int VPMacOSNativePlayerStepBackward(VPMacOSNativePlayer* player,
                                    char* error,
                                    size_t error_size);

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
int VPMacOSNativePlayerCopyPerfStats(
    VPMacOSNativePlayer* player,
    VPMacOSNativePlayerPerfStats* out);
int VPMacOSNativePlayerCopyAudioDiagnostics(
    VPMacOSNativePlayer* player,
    VPMacOSNativeAudioDiagnostics* out);
int VPMacOSNativeHardwareDecodeAvailable(void);
const char* VPMacOSNativeHardwareDecodeProviderName(void);

int VPMacOSMeasureBGRA(const uint8_t* bgra,
                       int32_t width,
                       int32_t height,
                       int32_t stride_bytes,
                       VPMacOSCaptureMetrics* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VOIDPLAYER_MACOS_NATIVE_PLAYER_BRIDGE_H_
