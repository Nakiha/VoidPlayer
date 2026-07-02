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
    let backendName = withUnsafeBytes(of: &state.backend_name) { rawBuffer -> String in
      guard let base = rawBuffer.bindMemory(to: CChar.self).baseAddress else {
        return "unknown"
      }
      let value = String(cString: base)
      return value.isEmpty ? "unknown" : value
    }
    let externalFlutterSurfaceLastError = withUnsafeBytes(
      of: &state.external_flutter_surface_last_error
    ) { rawBuffer -> String in
      guard let base = rawBuffer.bindMemory(to: CChar.self).baseAddress else {
        return "none"
      }
      let value = String(cString: base)
      return value.isEmpty ? "none" : value
    }
    let active = state.renderer_initialized != 0 &&
      state.target_installed != 0 &&
      state.backend_available != 0 &&
      state.last_draw_succeeded != 0
    var diagnostics: [String: Any] = [
      "rendererInitialized": state.renderer_initialized != 0,
      "targetInstalled": state.target_installed != 0,
      "backendAvailable": state.backend_available != 0,
      "backendName": backendName,
      "active": active,
      "lastDrawSucceeded": state.last_draw_succeeded != 0,
      "consecutiveDrawFailures": Int64(min(state.consecutive_draw_failures, UInt64(Int64.max))),
      "drawFailureCount": Int64(min(state.draw_failure_count, UInt64(Int64.max))),
      "uploadCount": Int64(min(state.upload_count, UInt64(Int64.max))),
      "uploadFailureCount": Int64(min(state.upload_failure_count, UInt64(Int64.max))),
      "uploadIntervalP95Ms": Int64(min(state.upload_interval_p95_ms, UInt64(Int64.max))),
      "targetGeneration": Int64(min(state.target_generation, UInt64(Int64.max))),
      "targetWarmupGeneration": Int64(
        min(state.target_warmup_generation, UInt64(Int64.max))
      ),
      "targetWarmupRemaining": Int64(
        min(state.target_warmup_remaining, UInt64(Int64.max))
      ),
      "targetWarmupSampleCount": Int64(
        min(state.target_warmup_sample_count, UInt64(Int64.max))
      ),
      "targetWarmupLastMs": Int64(min(state.target_warmup_last_ms, UInt64(Int64.max))),
      "targetWarmupP95Ms": Int64(min(state.target_warmup_p95_ms, UInt64(Int64.max))),
      "targetWidth": Int(state.target_width),
      "targetHeight": Int(state.target_height),
      "uploadStorageKind": Self.presentPackageStorageName(state.upload_storage_kind),
      "lastSuccessfulFramePtsUs": Int64(state.last_successful_frame_pts_us),
      "overlayLastExpected": state.overlay_last_expected != 0,
      "overlayLastApplied": state.overlay_last_applied != 0,
      "overlayLastFillRectCount": Int64(
        min(state.overlay_last_fill_rect_count, UInt64(Int64.max))
      ),
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
      "externalFlutterSurfaceGeneration": Int64(
        min(state.external_flutter_surface_generation, UInt64(Int64.max))
      ),
      "externalFlutterSurfaceConsumedGeneration": Int64(
        min(state.external_flutter_surface_consumed_generation, UInt64(Int64.max))
      ),
      "externalFlutterSurfaceUpdateCount": Int64(
        min(state.external_flutter_surface_update_count, UInt64(Int64.max))
      ),
      "externalFlutterSurfaceConsumeCount": Int64(
        min(state.external_flutter_surface_consume_count, UInt64(Int64.max))
      ),
      "externalFlutterSurfaceWaitCount": Int64(
        min(state.external_flutter_surface_wait_count, UInt64(Int64.max))
      ),
      "externalFlutterSurfaceWaitFailureCount": Int64(
        min(state.external_flutter_surface_wait_failure_count, UInt64(Int64.max))
      ),
      "externalFlutterSurfaceLastError": externalFlutterSurfaceLastError,
      "sourceCacheActive": state.source_cache_active != 0,
      "sourceCacheTextureCount": Int(state.source_cache_texture_count),
      "sourceCacheGeneration": Int64(
        min(state.source_cache_generation, UInt64(Int64.max))
      ),
      "sourceCachePublishCount": Int64(
        min(state.source_cache_publish_count, UInt64(Int64.max))
      ),
      "sourceProjectionActive": state.source_projection_active != 0,
      "sourceProjectionUpdateCount": Int64(
        min(state.source_projection_update_count, UInt64(Int64.max))
      ),
      "sourceProjectionConsumeCount": Int64(
        min(state.source_projection_consume_count, UInt64(Int64.max))
      ),
      "lastDrawError": lastError,
    ]
    diagnostics.merge(rendererOwnedLastFrameColorDiagnostics()) { _, next in next }
    return diagnostics
  }

  func rendererOwnedLastFrameColorDiagnostics() -> [String: Any] {
    var info = VPMacOSNativeFrameInfo()
    guard VPMacOSNativePlayerCopyLastRendererOwnedFrameInfo(handle, &info) == 0 else {
      return Self.emptyRendererOwnedLastFrameColorDiagnostics()
    }
    if let fallback = rendererOwnedTrackColorFallback() {
      if info.color_range == 0 {
        info.color_range = Int32(fallback.colorRange)
      }
      if info.color_matrix == 0 {
        info.color_matrix = Int32(fallback.colorMatrix)
      }
      if info.color_transfer == 0 {
        info.color_transfer = Int32(fallback.colorTransfer)
      }
      if info.color_primaries == 0 {
        info.color_primaries = Int32(fallback.colorPrimaries)
      }
    }
    return [
      "lastFrameColorRangeCode": Int(info.color_range),
      "lastFrameColorRange": Self.colorRangeName(info.color_range),
      "lastFrameColorMatrixCode": Int(info.color_matrix),
      "lastFrameColorMatrix": Self.colorMatrixName(info.color_matrix),
      "lastFrameColorTransferCode": Int(info.color_transfer),
      "lastFrameColorTransfer": Self.colorTransferName(info.color_transfer),
      "lastFrameColorPrimariesCode": Int(info.color_primaries),
      "lastFrameColorPrimaries": Self.colorPrimariesName(info.color_primaries),
    ]
  }

  private func rendererOwnedTrackColorFallback() -> (
    colorRange: Int,
    colorMatrix: Int,
    colorTransfer: Int,
    colorPrimaries: Int
  )? {
    guard let firstTrack = trackDiagnostics().first else {
      return nil
    }
    return (
      firstTrack["colorRangeCode"] as? Int ?? 0,
      firstTrack["colorMatrixCode"] as? Int ?? 0,
      firstTrack["colorTransferCode"] as? Int ?? 0,
      firstTrack["colorPrimariesCode"] as? Int ?? 0
    )
  }

  func performanceStats() -> [String: Any] {
    var stats = VPMacOSNativePlayerPerfStats()
    guard VPMacOSNativePlayerCopyPerfStats(handle, &stats) == 0 else {
      return [
        "processRssBytes": 0,
        "processPrivateBytes": 0,
        "dedicatedGpuUsageBytes": 0,
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
        "rendererDrawCount": 0,
        "rendererDrawAvgUs": 0,
        "rendererDrawMaxUs": 0,
        "rendererDrawP95Us": 0,
        "rendererDrawBackendAvgUs": 0,
        "rendererDrawBackendMaxUs": 0,
        "rendererDrawBackendP95Us": 0,
        "rendererLayoutIntentCount": 0,
        "rendererLayoutPresentedCount": 0,
        "rendererLayoutDeferredToPlaybackCount": 0,
        "rendererPlayingLayoutRedrawSuppressedCount": 0,
        "rendererLayoutStaleCompletionDropCount": 0,
        "rendererLastLayoutRevision": 0,
        "rendererLastPresentedLayoutRevision": 0,
        "rendererDrawsPerPresentedLayoutX1000": 0,
        "inFlightMetalBufferCount": 0,
        "metalBufferExhaustionCount": 0,
        "metalCommandCompletionP95Us": 0,
        "metalCommandFailureCount": 0,
        "wgpuComposeTotalP95Us": 0,
        "wgpuComposePreRenderP95Us": 0,
        "wgpuComposeImportP95Us": 0,
        "wgpuComposePrepareP95Us": 0,
        "wgpuComposeOverlayEncodeP95Us": 0,
        "wgpuComposeBindGroupP95Us": 0,
        "wgpuComposePassEncodeP95Us": 0,
        "wgpuComposeSubmitP95Us": 0,
        "wgpuComposeCpuRenderP95Us": 0,
        "asyncMetalPublishActive": false,
        "videoSourceUpdateCount": 0,
        "viewportCompositeCount": 0,
        "sourceFrameCacheHitCount": 0,
        "sourceFrameCacheMissCount": 0,
        "sourceFrameStaleCompletionDropCount": 0,
        "sourceFrameCacheHitRatioX1000": 0,
      ]
    }
    let maxInt64 = UInt64(Int64.max)
    return [
      "processRssBytes": Int64(min(stats.process_rss_bytes, maxInt64)),
      "processPrivateBytes": Int64(min(stats.process_private_bytes, maxInt64)),
      "dedicatedGpuUsageBytes": Int64(
        min(stats.dedicated_gpu_usage_bytes, maxInt64)
      ),
      "decodeFrameCount": Int64(min(UInt64(stats.decode_frame_count), maxInt64)),
      "decodeDroppedCount": Int64(min(UInt64(stats.decode_dropped_count), maxInt64)),
      "decodeElapsedMs": Int64(stats.decode_elapsed_ms),
      "decodeFps": stats.decode_fps,
      "decodeFpsX1000": Self.finiteNonNegativeX1000(stats.decode_fps),
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
      "rendererOwnedUploadFpsX1000": Self.finiteNonNegativeX1000(
        stats.renderer_owned_upload_fps
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
      "aggregateDecodeFpsX1000": Self.finiteNonNegativeX1000(stats.aggregate_decode_fps),
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
      "rendererDrawCount": Int64(min(UInt64(stats.renderer_draw_count), maxInt64)),
      "rendererDrawAvgUs": Int64(stats.renderer_draw_avg_us),
      "rendererDrawMaxUs": Int64(stats.renderer_draw_max_us),
      "rendererDrawP95Us": Int64(stats.renderer_draw_p95_us),
      "rendererDrawBackendAvgUs": Int64(stats.renderer_draw_backend_avg_us),
      "rendererDrawBackendMaxUs": Int64(stats.renderer_draw_backend_max_us),
      "rendererDrawBackendP95Us": Int64(stats.renderer_draw_backend_p95_us),
      "rendererLayoutIntentCount": Int64(
        min(UInt64(stats.renderer_layout_intent_count), maxInt64)
      ),
      "rendererLayoutPresentedCount": Int64(
        min(UInt64(stats.renderer_layout_presented_count), maxInt64)
      ),
      "rendererLayoutDeferredToPlaybackCount": Int64(
        min(UInt64(stats.renderer_layout_deferred_to_playback_count), maxInt64)
      ),
      "rendererPlayingLayoutRedrawSuppressedCount": Int64(
        min(UInt64(stats.renderer_playing_layout_redraw_suppressed_count), maxInt64)
      ),
      "rendererLayoutStaleCompletionDropCount": Int64(
        min(UInt64(stats.renderer_layout_stale_completion_drop_count), maxInt64)
      ),
      "rendererLastLayoutRevision": Int64(
        min(UInt64(stats.renderer_last_layout_revision), maxInt64)
      ),
      "rendererLastPresentedLayoutRevision": Int64(
        min(UInt64(stats.renderer_last_presented_layout_revision), maxInt64)
      ),
      "rendererDrawsPerPresentedLayoutX1000": Int64(
        stats.renderer_draws_per_presented_layout_x1000
      ),
      "inFlightMetalBufferCount": Int64(
        min(UInt64(stats.in_flight_metal_buffer_count), maxInt64)
      ),
      "metalBufferExhaustionCount": Int64(
        min(UInt64(stats.metal_buffer_exhaustion_count), maxInt64)
      ),
      "metalCommandCompletionP95Us": Int64(
        min(UInt64(stats.metal_command_completion_p95_us), maxInt64)
      ),
      "metalCommandFailureCount": Int64(
        min(UInt64(stats.metal_command_failure_count), maxInt64)
      ),
      "wgpuComposeTotalP95Us": Int64(
        min(UInt64(stats.wgpu_compose_total_p95_us), maxInt64)
      ),
      "wgpuComposePreRenderP95Us": Int64(
        min(UInt64(stats.wgpu_compose_pre_render_p95_us), maxInt64)
      ),
      "wgpuComposeImportP95Us": Int64(
        min(UInt64(stats.wgpu_compose_import_p95_us), maxInt64)
      ),
      "wgpuComposePrepareP95Us": Int64(
        min(UInt64(stats.wgpu_compose_prepare_p95_us), maxInt64)
      ),
      "wgpuComposeOverlayEncodeP95Us": Int64(
        min(UInt64(stats.wgpu_compose_overlay_encode_p95_us), maxInt64)
      ),
      "wgpuComposeBindGroupP95Us": Int64(
        min(UInt64(stats.wgpu_compose_bind_group_p95_us), maxInt64)
      ),
      "wgpuComposePassEncodeP95Us": Int64(
        min(UInt64(stats.wgpu_compose_pass_encode_p95_us), maxInt64)
      ),
      "wgpuComposeSubmitP95Us": Int64(
        min(UInt64(stats.wgpu_compose_submit_p95_us), maxInt64)
      ),
      "wgpuComposeCpuRenderP95Us": Int64(
        min(UInt64(stats.wgpu_compose_cpu_render_p95_us), maxInt64)
      ),
      "asyncMetalPublishActive": stats.async_metal_publish_active != 0,
      "videoSourceUpdateCount": Int64(
        min(UInt64(stats.video_source_update_count), maxInt64)
      ),
      "viewportCompositeCount": Int64(
        min(UInt64(stats.viewport_composite_count), maxInt64)
      ),
      "sourceFrameCacheHitCount": Int64(
        min(UInt64(stats.source_frame_cache_hit_count), maxInt64)
      ),
      "sourceFrameCacheMissCount": Int64(
        min(UInt64(stats.source_frame_cache_miss_count), maxInt64)
      ),
      "sourceFrameStaleCompletionDropCount": Int64(
        min(UInt64(stats.source_frame_stale_completion_drop_count), maxInt64)
      ),
      "sourceFrameCacheHitRatioX1000": Self.ratioX1000(
        numerator: stats.source_frame_cache_hit_count,
        denominator: Self.saturatingAdd(
          stats.source_frame_cache_hit_count,
          stats.source_frame_cache_miss_count
        )
      ),
    ]
  }

  private static func ratioX1000(numerator: UInt64, denominator: UInt64) -> Int64 {
    guard denominator > 0 else { return 0 }
    let quotient = numerator / denominator
    let remainder = numerator % denominator
    let scaledQuotient = min(quotient, UInt64(Int64.max) / 1000) * 1000
    let scaledRemainder = min(remainder, UInt64.max / 1000) * 1000 / denominator
    return Int64(min(scaledQuotient + scaledRemainder, UInt64(Int64.max)))
  }

  private static func finiteNonNegativeX1000(_ value: Double) -> Int64 {
    guard value.isFinite, value > 0 else { return 0 }
    let scaledLimit = Double(Int64.max) / 1000.0
    guard value < scaledLimit else { return Int64.max }
    return Int64(value * 1000.0)
  }

  private static func saturatingAdd(_ lhs: UInt64, _ rhs: UInt64) -> UInt64 {
    let (sum, overflow) = lhs.addingReportingOverflow(rhs)
    return overflow ? UInt64.max : sum
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
        "decodeFpsX1000": Self.finiteNonNegativeX1000(track.decode_fps),
        "decodeAvgMs": track.decode_avg_ms,
        "decodeMaxMs": track.decode_max_ms,
        "currentPtsUs": Int64(track.current_pts_us),
        "currentDtsUs": Int64(track.current_dts_us),
        "ptsUs": Int64(track.current_pts_us),
        "dtsUs": Int64(track.current_dts_us),
        "analysisFrameIndex": Int(track.analysis_frame_index),
        "frameIdentityMode": Int(track.frame_identity_mode),
        "sourcePacketIndex": Int(track.source_packet_index),
        "sourcePacketSize": Int(track.source_packet_size),
        "sourcePacketPos": Int64(track.source_packet_pos),
        "sourcePacketPtsUs": Int64(track.source_packet_pts),
        "sourcePacketDtsUs": Int64(track.source_packet_dts),
        "colorRangeCode": Int(track.color_range),
        "colorRange": Self.colorRangeName(track.color_range),
        "colorMatrixCode": Int(track.color_matrix),
        "colorMatrix": Self.colorMatrixName(track.color_matrix),
        "colorTransferCode": Int(track.color_transfer),
        "colorTransfer": Self.colorTransferName(track.color_transfer),
        "colorPrimariesCode": Int(track.color_primaries),
        "colorPrimaries": Self.colorPrimariesName(track.color_primaries),
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
    case Int32(VPMacOSNativePresentPackageStorageSourceOutputAtlas):
      return "source-output-atlas"
    default:
      return "unavailable"
    }
  }

  private static func colorRangeName(_ value: Int32) -> String {
    switch value {
    case 1:
      return "limited"
    case 2:
      return "full"
    default:
      return "unknown"
    }
  }

  private static func colorMatrixName(_ value: Int32) -> String {
    switch value {
    case 1:
      return "bt601"
    case 2:
      return "bt709"
    case 3:
      return "bt2020-ncl"
    default:
      return "unknown"
    }
  }

  private static func colorTransferName(_ value: Int32) -> String {
    switch value {
    case 1:
      return "sdr"
    case 2:
      return "pq"
    case 3:
      return "hlg"
    default:
      return "unknown"
    }
  }

  private static func colorPrimariesName(_ value: Int32) -> String {
    switch value {
    case 1:
      return "bt601"
    case 2:
      return "bt709"
    case 3:
      return "bt2020"
    default:
      return "unknown"
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
      "uploadIntervalP95Ms": 0,
      "targetGeneration": 0,
      "targetWarmupGeneration": 0,
      "targetWarmupRemaining": 0,
      "targetWarmupSampleCount": 0,
      "targetWarmupLastMs": 0,
      "targetWarmupP95Ms": 0,
      "targetWidth": 0,
      "targetHeight": 0,
      "uploadStorageKind": "unavailable",
      "lastSuccessfulFramePtsUs": 0,
      "lastFrameColorRangeCode": 0,
      "lastFrameColorRange": "unknown",
      "lastFrameColorMatrixCode": 0,
      "lastFrameColorMatrix": "unknown",
      "lastFrameColorTransferCode": 0,
      "lastFrameColorTransfer": "unknown",
      "lastFrameColorPrimariesCode": 0,
      "lastFrameColorPrimaries": "unknown",
      "overlayLastExpected": false,
      "overlayLastApplied": false,
      "overlayLastFillRectCount": 0,
      "overlayLastLineRectCount": 0,
      "overlayExpectedCount": 0,
      "overlayAppliedCount": 0,
      "overlayMissedCount": 0,
      "overlayGpuSuccessCount": 0,
      "overlayGpuFailureCount": 0,
      "overlayCpuFallbackCount": 0,
      "externalFlutterSurfaceGeneration": 0,
      "externalFlutterSurfaceConsumedGeneration": 0,
      "externalFlutterSurfaceUpdateCount": 0,
      "externalFlutterSurfaceConsumeCount": 0,
      "externalFlutterSurfaceWaitCount": 0,
      "externalFlutterSurfaceWaitFailureCount": 0,
      "externalFlutterSurfaceLastError": "none",
      "sourceCacheActive": false,
      "sourceCacheTextureCount": 0,
      "sourceCacheGeneration": 0,
      "sourceCachePublishCount": 0,
      "sourceProjectionActive": false,
      "sourceProjectionUpdateCount": 0,
      "sourceProjectionConsumeCount": 0,
      "backendName": "unknown",
      "lastDrawError": "",
    ]
  }

  private static func emptyRendererOwnedLastFrameColorDiagnostics() -> [String: Any] {
    [
      "lastFrameColorRangeCode": 0,
      "lastFrameColorRange": "unknown",
      "lastFrameColorMatrixCode": 0,
      "lastFrameColorMatrix": "unknown",
      "lastFrameColorTransferCode": 0,
      "lastFrameColorTransfer": "unknown",
      "lastFrameColorPrimariesCode": 0,
      "lastFrameColorPrimaries": "unknown",
    ]
  }
}
