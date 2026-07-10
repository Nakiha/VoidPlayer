#ifndef VOIDPLAYER_MACOS_NATIVE_PLAYER_BRIDGE_H_
#define VOIDPLAYER_MACOS_NATIVE_PLAYER_BRIDGE_H_

#include "macos/metal/metal_presentation_backend_bridge.h"
#include "native_player_types.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VPMacOSNativeSourceCompositorLease
    VPMacOSNativeSourceCompositorLease;

/*
 * macOS native bridge contract:
 *
 * - This C ABI is embedded by the macOS Flutter runner; it is not a stable
 *   system-wide plugin ABI. VP_MACOS_NATIVE_API_VERSION must be bumped when
 *   existing struct layouts or ownership rules become incompatible.
 * - VPMacOSNativePlayer* is owned by the caller. Create/Destroy and all player
 *   control functions may be called from the main thread or automation thread,
 *   but concurrent calls for the same player are serialized internally only at
 *   the player boundary.
 * - `const char* path` arguments are borrowed for the duration of the call.
 * - `void* pixel_buffer` arguments are borrowed CVPixelBufferRef values. The
 *   caller must keep the pixel buffer alive until it is replaced or
 *   VPMacOSNativePlayerClearMetalPresentationTarget returns.
 * - The frame callback may run on a renderer/native worker thread. It must not
 *   call back into player APIs synchronously. Clearing the callback with
 *   VPMacOSNativePlayerSetFrameAvailableCallback(player, NULL, NULL) waits for
 *   any callback invocation already in progress to return. Destroy also clears
 *   and drains the callback before freeing the player.
 * - Error buffers are caller-owned writable byte buffers. On success, functions
 *   that accept an error buffer write an empty string when possible.
 */

void VPMacOSInstallCrashHandler(const char* crash_dir);
void VPMacOSRemoveCrashHandler(void);
void VPMacOSConfigureLogging(const char* logs_dir,
                             const char* log_file_name,
                             const char* level);
void VPMacOSLogProfilerSummary(const char* message);
void VPMacOSNativeAnalysisOverlayClearTracks(void);
int VPMacOSNativeAnalysisOverlaySetTrack(int32_t track_file_id,
                                         const char* analysis_path);
void VPMacOSNativeAnalysisOverlaySetState(int32_t show_cu_grid,
                                          int32_t show_pred_mode,
                                          int32_t show_qp_heatmap,
                                          int32_t show_pred_lines,
                                          int32_t show_cu_bit_cost_heatmap,
                                          int32_t opacity_permille,
                                          int32_t mode,
                                          int32_t track_file_id);

VPMacOSNativePlayer* VPMacOSNativePlayerCreate(void);
void VPMacOSNativePlayerDestroy(VPMacOSNativePlayer* player);

int VPMacOSNativePlayerOpen(VPMacOSNativePlayer* player,
                            const char* path,
                            char* error,
                            size_t error_size);
int VPMacOSNativeProbeTrackInfo(const char* path,
                                VPMacOSNativeTrackInfo* out,
                                char* error,
                                size_t error_size);
int VPMacOSNativePlayerAddTrack(VPMacOSNativePlayer* player,
                                const char* path,
                                int32_t file_id,
                                int use_hardware_decode,
                                VPMacOSNativeTrackInfo* out,
                                char* error,
                                size_t error_size);
int VPMacOSNativePlayerCopyTrackInfo(VPMacOSNativePlayer* player,
                                     int32_t file_id,
                                     VPMacOSNativeTrackInfo* out);
void VPMacOSNativePlayerRemoveTrack(VPMacOSNativePlayer* player,
                                    int32_t file_id);
void VPMacOSNativePlayerClose(VPMacOSNativePlayer* player);
void VPMacOSNativePlayerSetHardwareDecodeEnabled(VPMacOSNativePlayer* player,
                                                 int enabled);
void VPMacOSNativePlayerSetBackgroundColor(VPMacOSNativePlayer* player,
                                           float r,
                                           float g,
                                           float b,
                                           float a);
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
int VPMacOSNativePlayerInstallMetalPresentationTargetRing(
    VPMacOSNativePlayer* player,
    VPMacOSMetalPresentationBackend* backend,
    const void* const* pixel_buffers,
    size_t pixel_buffer_count,
    void* displayed_pixel_buffer,
    void* protected_pixel_buffer,
    int32_t width,
    int32_t height,
    int32_t max_track_slots);
void VPMacOSNativePlayerMarkMetalPresentationTargetDisplayed(
    VPMacOSNativePlayer* player,
    void* pixel_buffer);
void VPMacOSNativePlayerProtectMetalPresentationTarget(
    VPMacOSNativePlayer* player,
    void* pixel_buffer);
void VPMacOSNativePlayerReleaseMetalPresentationTarget(
    VPMacOSNativePlayer* player,
    void* pixel_buffer);
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
enum {
  VPMacOSNativeFrameRefreshSuppressFrameCallback = 1u << 0,
};
int VPMacOSNativePlayerRequestRendererOwnedFrameRefresh(
    VPMacOSNativePlayer* player,
    int32_t timeout_ms,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);
int VPMacOSNativePlayerRequestRendererOwnedFrameRefreshWithOptions(
    VPMacOSNativePlayer* player,
    int32_t timeout_ms,
    uint32_t flags,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);
int VPMacOSNativePlayerCommitSourceProviderPreview(
    VPMacOSNativePlayer* player,
    int32_t timeout_ms,
    const int32_t* expected_file_ids,
    size_t expected_file_id_count,
    VPMacOSNativeFrameInfo* out,
    char* error,
    size_t error_size);
int VPMacOSNativePlayerBakeCurrentFrameSources(
    VPMacOSNativePlayer* player,
    VPMacOSMetalPresentationBackend* backend,
    VPMacOSNativeSourceFrameBakeTarget* targets,
    size_t target_count,
    char* error,
    size_t error_size);
VPMacOSNativeSourceCompositorLease*
VPMacOSNativeSourceCompositorLeaseCreate(void);
void VPMacOSNativeSourceCompositorLeaseDestroy(
    VPMacOSNativeSourceCompositorLease* lease);
int VPMacOSNativeSourceCompositorLeaseSubscribeAndBake(
    VPMacOSNativeSourceCompositorLease* lease,
    VPMacOSNativePlayer* player,
    const VPMacOSNativeSourceCompositorDescriptor* descriptors,
    size_t descriptor_count,
    int32_t edr_output_enabled,
    uint64_t topology_generation,
    VPMacOSNativeSourceCompositorPackage* out,
    char* error,
    size_t error_size);
int VPMacOSNativeSourceCompositorLeaseRefreshAndBake(
    VPMacOSNativeSourceCompositorLease* lease,
    VPMacOSNativePlayer* player,
    VPMacOSNativeSourceCompositorPackage* out,
    char* error,
    size_t error_size);
int VPMacOSNativeSourceCompositorLeaseHasCompletePackage(
    VPMacOSNativeSourceCompositorLease* lease,
    uint64_t topology_generation);
void VPMacOSNativeSourceCompositorLeaseReset(
    VPMacOSNativeSourceCompositorLease* lease);
int VPMacOSNativeSourceCompositorLeaseCopyDiagnostics(
    VPMacOSNativeSourceCompositorLease* lease,
    VPMacOSNativeSourceCompositorDiagnostics* out);
int VPMacOSNativePlayerCopyCurrentOverlayPrimitives(
    VPMacOSNativePlayer* player,
    VPMacOSNativeOverlayPrimitiveSnapshot* snapshot,
    VPMacOSNativeOverlayGpuRect* fill_rects,
    size_t fill_rect_capacity,
    VPMacOSNativeOverlayGpuRect* line_rects,
    size_t line_rect_capacity,
    VPMacOSNativeOverlayGpuRect* motion_lines,
    size_t motion_line_capacity,
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
void VPMacOSNativePlayerNoteViewportCompositorActivity(VPMacOSNativePlayer* player);
void VPMacOSNativePlayerSetViewportCompositorActive(VPMacOSNativePlayer* player,
                                                    int active);
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
