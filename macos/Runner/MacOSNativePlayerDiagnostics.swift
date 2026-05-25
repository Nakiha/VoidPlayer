import Foundation

extension MacOSNativePlayerSession {
  func presentationSchedulerStats() -> [String: Any] {
    var stats = VPMacOSNativePresentationSchedulerStats()
    guard VPMacOSNativePlayerCopyPresentationSchedulerStats(handle, &stats) == 0 else {
      return [
        "tickCount": 0,
        "presentableTickCount": 0,
        "frameNotificationCount": 0,
        "lastSelectedPtsUs": -1,
        "lastPresentFrameCount": 0,
        "cachedPresentDecisionAvailable": false,
        "deadlineSleepCount": 0,
        "lastDeadlineSleepUs": 0,
      ]
    }
    let maxInt64 = UInt64(Int64.max)
    return [
      "tickCount": Int64(min(UInt64(stats.tick_count), maxInt64)),
      "presentableTickCount": Int64(min(UInt64(stats.presentable_tick_count), maxInt64)),
      "frameNotificationCount": Int64(min(UInt64(stats.frame_notification_count), maxInt64)),
      "lastSelectedPtsUs": Int64(stats.last_selected_pts_us),
      "lastPresentFrameCount": Int(stats.last_present_frame_count),
      "cachedPresentDecisionAvailable": stats.cached_present_decision_available != 0,
      "deadlineSleepCount": Int64(min(UInt64(stats.deadline_sleep_count), maxInt64)),
      "lastDeadlineSleepUs": Int64(stats.last_deadline_sleep_us),
    ]
  }

  func performanceStats() -> [String: Any] {
    var stats = VPMacOSNativePlayerPerfStats()
    guard VPMacOSNativePlayerCopyPerfStats(handle, &stats) == 0 else {
      return [
        "decodeFrameCount": 0,
        "decodeDroppedCount": 0,
        "decodeElapsedMs": 0,
        "decodeFps": 0.0,
        "decodeFpsX1000": 0,
        "decodeAvgMs": 0.0,
        "decodeMaxMs": 0.0,
        "rendererOwnedUploadCount": 0,
        "rendererOwnedUploadFailureCount": 0,
        "rendererOwnedUploadElapsedMs": 0,
        "rendererOwnedUploadFps": 0.0,
        "rendererOwnedUploadFpsX1000": 0,
        "rendererOwnedDirectYuvUploadCount": 0,
        "rendererOwnedCVPixelBufferUploadCount": 0,
        "rendererOwnedPresentPackageUploadCount": 0,
        "rendererOwnedPresentPackageCopyUs": 0,
        "rendererOwnedPresentPackageGpuWaitUs": 0,
        "rendererOwnedPresentPackageTotalUs": 0,
        "rendererOwnedPresentPackageStorage": "unavailable",
      ]
    }
    let maxInt64 = UInt64(Int64.max)
    return [
      "decodeFrameCount": Int64(min(UInt64(stats.decode_frame_count), maxInt64)),
      "decodeDroppedCount": Int64(min(UInt64(stats.decode_dropped_count), maxInt64)),
      "decodeElapsedMs": Int64(stats.decode_elapsed_ms),
      "decodeFps": stats.decode_fps,
      "decodeFpsX1000": Int64(max(0.0, stats.decode_fps * 1000.0)),
      "decodeAvgMs": stats.decode_avg_ms,
      "decodeMaxMs": stats.decode_max_ms,
      "rendererOwnedUploadCount": Int64(
        min(UInt64(stats.renderer_owned_upload_count), maxInt64)
      ),
      "rendererOwnedUploadFailureCount": Int64(
        min(UInt64(stats.renderer_owned_upload_failure_count), maxInt64)
      ),
      "rendererOwnedUploadElapsedMs": Int64(stats.renderer_owned_upload_elapsed_ms),
      "rendererOwnedUploadFps": stats.renderer_owned_upload_fps,
      "rendererOwnedUploadFpsX1000": Int64(
        max(0.0, stats.renderer_owned_upload_fps * 1000.0)
      ),
      "rendererOwnedDirectYuvUploadCount": Int64(
        stats.renderer_owned_direct_yuv_upload_count
      ),
      "rendererOwnedCVPixelBufferUploadCount": Int64(
        stats.renderer_owned_cvpixelbuffer_upload_count
      ),
      "rendererOwnedPresentPackageUploadCount": Int64(
        stats.renderer_owned_present_package_upload_count
      ),
      "rendererOwnedPresentPackageCopyUs": Int64(
        stats.renderer_owned_present_package_copy_us
      ),
      "rendererOwnedPresentPackageGpuWaitUs": Int64(
        stats.renderer_owned_present_package_gpu_wait_us
      ),
      "rendererOwnedPresentPackageTotalUs": Int64(
        stats.renderer_owned_present_package_total_us
      ),
      "rendererOwnedPresentPackageStorage": Self.presentPackageStorageName(
        stats.renderer_owned_present_package_storage
      ),
    ]
  }

  private static func presentPackageStorageName(_ storage: Int32) -> String {
    switch storage {
    case Int32(VPMacOSNativePresentPackageStorageYUV):
      return "yuv"
    case Int32(VPMacOSNativePresentPackageStorageBGRA):
      return "bgra"
    case Int32(VPMacOSNativePresentPackageStorageCVPixelBuffer):
      return "cvpixelbuffer"
    default:
      return "unavailable"
    }
  }
}
