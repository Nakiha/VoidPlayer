import Foundation

extension MacOSNativePlayerSession {
  func hardwareDecodeActive() -> Bool {
    VPMacOSNativePlayerHardwareDecodeActive(handle) != 0
  }

  func hardwareDecodeDownloadsToCpu() -> Bool {
    VPMacOSNativePlayerHardwareDecodeDownloadsToCpu(handle) != 0
  }

  func decodeModeName() -> String {
    String(cString: VPMacOSNativePlayerDecodeModeName(handle))
  }

  func decoderName() -> String {
    String(cString: VPMacOSNativePlayerDecoderName(handle))
  }

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

  func rendererOwnedPresentationState() -> [String: Any] {
    var state = VPMacOSNativeRendererOwnedPresentationState()
    guard VPMacOSNativePlayerCopyRendererOwnedPresentationState(handle, &state) == 0 else {
      return Self.emptyRendererOwnedPresentationState()
    }
    let lastError = withUnsafeBytes(of: &state.last_draw_error) { rawBuffer -> String in
      guard let base = rawBuffer.bindMemory(to: CChar.self).baseAddress else {
        return ""
      }
      return String(cString: base)
    }
    let active = state.renderer_initialized != 0 &&
      state.target_installed != 0 &&
      state.backend_available != 0 &&
      state.last_draw_succeeded != 0
    return [
      "rendererInitialized": state.renderer_initialized != 0,
      "targetInstalled": state.target_installed != 0,
      "backendAvailable": state.backend_available != 0,
      "active": active,
      "lastDrawSucceeded": state.last_draw_succeeded != 0,
      "consecutiveDrawFailures": Int64(min(state.consecutive_draw_failures, UInt64(Int64.max))),
      "drawFailureCount": Int64(min(state.draw_failure_count, UInt64(Int64.max))),
      "uploadCount": Int64(min(state.upload_count, UInt64(Int64.max))),
      "uploadFailureCount": Int64(min(state.upload_failure_count, UInt64(Int64.max))),
      "targetGeneration": Int64(min(state.target_generation, UInt64(Int64.max))),
      "targetWidth": Int(state.target_width),
      "targetHeight": Int(state.target_height),
      "uploadStorageKind": Self.presentPackageStorageName(state.upload_storage_kind),
      "lastSuccessfulFramePtsUs": Int64(state.last_successful_frame_pts_us),
      "overlayLastExpected": state.overlay_last_expected != 0,
      "overlayLastApplied": state.overlay_last_applied != 0,
      "overlayLastLineRectCount": Int64(
        min(state.overlay_last_line_rect_count, UInt64(Int64.max))
      ),
      "overlayExpectedCount": Int64(
        min(state.overlay_expected_count, UInt64(Int64.max))
      ),
      "overlayAppliedCount": Int64(
        min(state.overlay_applied_count, UInt64(Int64.max))
      ),
      "overlayMissedCount": Int64(min(state.overlay_missed_count, UInt64(Int64.max))),
      "overlayGpuSuccessCount": Int64(
        min(state.overlay_gpu_success_count, UInt64(Int64.max))
      ),
      "overlayGpuFailureCount": Int64(
        min(state.overlay_gpu_failure_count, UInt64(Int64.max))
      ),
      "overlayCpuFallbackCount": Int64(
        min(state.overlay_cpu_fallback_count, UInt64(Int64.max))
      ),
      "lastDrawError": lastError,
    ]
  }

  func performanceStats() -> [String: Any] {
    var stats = VPMacOSNativePlayerPerfStats()
    guard VPMacOSNativePlayerCopyPerfStats(handle, &stats) == 0 else {
      return [
        "processRssBytes": 0,
        "processPrivateBytes": 0,
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
        "activeTrackCount": 0,
        "aggregateDecodeFrameCount": 0,
        "aggregateDecodeFps": 0.0,
        "aggregateDecodeFpsX1000": 0,
        "cpuFrameMemoryBytes": 0,
        "packetQueueMemoryBytes": 0,
        "rendererOwnedStagingAllocationCount": 0,
        "rendererOwnedStagingReuseCount": 0,
        "rendererOwnedStagingMaxBytes": 0,
      ]
    }
    let maxInt64 = UInt64(Int64.max)
    return [
      "processRssBytes": Int64(min(stats.process_rss_bytes, maxInt64)),
      "processPrivateBytes": Int64(min(stats.process_private_bytes, maxInt64)),
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
      "activeTrackCount": Int64(min(UInt64(stats.active_track_count), maxInt64)),
      "aggregateDecodeFrameCount": Int64(
        min(UInt64(stats.aggregate_decode_frame_count), maxInt64)
      ),
      "aggregateDecodeFps": stats.aggregate_decode_fps,
      "aggregateDecodeFpsX1000": Int64(max(0.0, stats.aggregate_decode_fps * 1000.0)),
      "cpuFrameMemoryBytes": Int64(min(stats.cpu_frame_memory_bytes, maxInt64)),
      "packetQueueMemoryBytes": Int64(min(stats.packet_queue_memory_bytes, maxInt64)),
      "rendererOwnedStagingAllocationCount": Int64(
        min(UInt64(stats.renderer_owned_staging_allocation_count), maxInt64)
      ),
      "rendererOwnedStagingReuseCount": Int64(
        min(UInt64(stats.renderer_owned_staging_reuse_count), maxInt64)
      ),
      "rendererOwnedStagingMaxBytes": Int64(
        min(UInt64(stats.renderer_owned_staging_max_bytes), maxInt64)
      ),
    ]
  }

  func trackDiagnostics() -> [[String: Any]] {
    var count: Int = 0
    _ = VPMacOSNativePlayerCopyTrackDiagnostics(handle, nil, 0, &count)
    guard count > 0 else { return [] }
    var tracks = Array(
      repeating: VPMacOSNativeTrackDiagnosticInfo(),
      count: min(count, Int(VPMacOSNativeMaxTracks))
    )
    var copiedCount: Int = 0
    let ret = tracks.withUnsafeMutableBufferPointer { buffer in
      VPMacOSNativePlayerCopyTrackDiagnostics(
        handle,
        buffer.baseAddress,
        buffer.count,
        &copiedCount
      )
    }
    guard ret == 0 else { return [] }
    return tracks.prefix(min(copiedCount, tracks.count)).map { track in
      [
        "fileId": Int(track.file_id),
        "slot": Int(track.slot),
        "width": Int(track.width),
        "height": Int(track.height),
        "durationUs": Int64(track.duration_us),
        "offsetUs": Int64(track.offset_us),
        "hardwareDecodeActive": track.hardware_decode_active != 0,
        "hardwareDecodeDownloadsToCpu": track.hardware_decode_downloads_to_cpu != 0,
        "bufferState": Int(track.buffer_state),
        "bufferCount": Int64(min(track.buffer_count, UInt64(Int64.max))),
        "bufferCapacity": Int64(min(track.buffer_capacity, UInt64(Int64.max))),
        "cpuFrameMemoryBytes": Int64(min(track.cpu_frame_memory_bytes, UInt64(Int64.max))),
        "packetQueueMemoryBytes": Int64(
          min(track.packet_queue_memory_bytes, UInt64(Int64.max))
        ),
        "framesDecoded": Int64(min(track.frames_decoded, UInt64(Int64.max))),
        "decodeFps": track.decode_fps,
        "decodeFpsX1000": Int64(max(0.0, track.decode_fps * 1000.0)),
        "decodeAvgMs": track.decode_avg_ms,
        "decodeMaxMs": track.decode_max_ms,
        "currentPtsUs": Int64(track.current_pts_us),
        "currentDtsUs": Int64(track.current_dts_us),
        "codecName": Self.cString(track.codec_name),
        "decoderName": Self.cString(track.decoder_name),
        "decodeMode": Self.cString(track.decode_mode),
      ]
    }
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

  private static func cString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { rawBuffer -> String in
      guard let base = rawBuffer.bindMemory(to: CChar.self).baseAddress else {
        return ""
      }
      return String(cString: base)
    }
  }

  private static func emptyRendererOwnedPresentationState() -> [String: Any] {
    [
      "rendererInitialized": false,
      "targetInstalled": false,
      "backendAvailable": false,
      "active": false,
      "lastDrawSucceeded": false,
      "consecutiveDrawFailures": 0,
      "drawFailureCount": 0,
      "uploadCount": 0,
      "uploadFailureCount": 0,
      "targetGeneration": 0,
      "targetWidth": 0,
      "targetHeight": 0,
      "uploadStorageKind": "unavailable",
      "lastSuccessfulFramePtsUs": 0,
      "overlayLastExpected": false,
      "overlayLastApplied": false,
      "overlayLastLineRectCount": 0,
      "overlayExpectedCount": 0,
      "overlayAppliedCount": 0,
      "overlayMissedCount": 0,
      "overlayGpuSuccessCount": 0,
      "overlayGpuFailureCount": 0,
      "overlayCpuFallbackCount": 0,
      "lastDrawError": "",
    ]
  }
}
