import Foundation

enum MacOSVideoRendererDiagnostics {
  static func map(
    backendName: String,
    player: MacOSNativePlayerSession?,
    playerId: Int64?,
    textureId: Int64?,
    textureStats: (
      rebuildCount: Int,
      reuseCount: Int,
      allocationCount: Int,
      rebuildReuseCount: Int,
      rebuildLastAllocatedCount: Int,
      rebuildLastReusedCount: Int,
      rebuildLastDurationMs: Double,
      retiredPixelBufferCount: Int,
      retiredPixelBufferBytes: Int,
      prewarmRequestCount: Int,
      prewarmHitCount: Int,
      prewarmReadyCount: Int,
      prewarmDroppedCount: Int,
      metalUploadCount: Int,
      metalUploadFailureCount: Int,
      metalAvailable: Bool,
      metalTextureCacheAvailable: Bool,
      metalTextureValid: Bool,
      metalTextureCreationCount: Int,
      metalTextureFailureCount: Int,
      metalTextureLastError: String,
      nativeTargetPixelBufferBytes: Int,
      nativeTargetPixelBufferCount: Int,
      inFlightMetalBufferCount: Int,
      metalBufferExhaustionCount: Int,
      stableDisplayFallbackActive: Bool,
      stableDisplayFallbackCount: Int,
      stableDisplayFallbackPtsUs: Int
    )?,
    textureDimensions: (width: Int, height: Int)?,
    trackCount: Int,
    isPlaying: Bool,
    presentationTargetInstalled: Bool,
    nativeEventDiagnostics: [String: Any],
    frameCallbackDiagnostics: [String: Any],
    viewportDiagnostics: [String: Any],
    presentationDiagnostics: [String: Any],
    trackPayloads: [[String: Any]] = []
  ) -> [String: Any] {
    let layoutSnapshot = player?.layoutSnapshotMap()
    let schedulerStats = player?.presentationSchedulerStats()
    let perfStats = player?.performanceStats()
    let audioDiagnostics = player?.audioDiagnostics() ?? [:]
    let trackDiagnostics = player?.trackDiagnostics() ?? []
    let primaryTrack = trackDiagnostics.first
    let primaryTrackPayload = trackPayloads.first
    let secondaryTrack = trackDiagnostics.dropFirst().first
    let nativeTargetState = player?.nativeTargetPresentationState()
      ?? Self.emptyNativeTargetPresentationState()
    let nativeTargetLastFrameColorRangeCode = colorDiagnosticCode(
      state: nativeTargetState,
      stateKey: "lastFrameColorRangeCode",
      fallbackTrack: primaryTrackPayload,
      trackKey: "colorRange"
    )
    let nativeTargetLastFrameColorMatrixCode = colorDiagnosticCode(
      state: nativeTargetState,
      stateKey: "lastFrameColorMatrixCode",
      fallbackTrack: primaryTrackPayload,
      trackKey: "colorMatrix"
    )
    let nativeTargetLastFrameColorTransferCode = colorDiagnosticCode(
      state: nativeTargetState,
      stateKey: "lastFrameColorTransferCode",
      fallbackTrack: primaryTrackPayload,
      trackKey: "colorTransfer"
    )
    let nativeTargetLastFrameColorPrimariesCode = colorDiagnosticCode(
      state: nativeTargetState,
      stateKey: "lastFrameColorPrimariesCode",
      fallbackTrack: primaryTrackPayload,
      trackKey: "colorPrimaries"
    )
    let nativeTargetActive = nativeTargetState["active"] as? Bool ?? false
    let rendererDrawCount = int64Diagnostic(perfStats?["rendererDrawCount"])
    let nativeFramePresentationCount =
      int64Diagnostic(presentationDiagnostics["nativeFramePresentationCount"])
    let drawsPerPresentedFrameRatioX1000 = nativeFramePresentationCount > 0
      ? rendererDrawCount * 1000 / nativeFramePresentationCount
      : int64Diagnostic(perfStats?["rendererDrawsPerPresentedLayoutX1000"])
    let nativeInFlightMetalBufferCount =
      int64Diagnostic(perfStats?["inFlightMetalBufferCount"])
    let textureInFlightMetalBufferCount =
      Int64(textureStats?.inFlightMetalBufferCount ?? 0)
    let nativeMetalBufferExhaustionCount =
      int64Diagnostic(perfStats?["metalBufferExhaustionCount"])
    let textureMetalBufferExhaustionCount =
      Int64(textureStats?.metalBufferExhaustionCount ?? 0)
    let nativeDedicatedGpuUsageBytes = int64Diagnostic(perfStats?["dedicatedGpuUsageBytes"])
    let nativeTargetPixelBufferBytes =
      Int64(textureStats?.nativeTargetPixelBufferBytes ?? 0)
    let dedicatedGpuUsageBytes = nativeDedicatedGpuUsageBytes + nativeTargetPixelBufferBytes
    var diagnostics: [String: Any] = [
      "platform": "macos",
      "backend": backendName,
      "presentationAdapter": String(cString: VPMacOSNativePresentationAdapterName()),
      "presentationAdapterKind": presentationAdapterKind(
        player: player,
        state: nativeTargetState
      ),
      "presentationScheduler": String(cString: VPMacOSNativePresentationSchedulerName()),
      "presentationBackend": presentationBackendName(
        player: player,
        state: nativeTargetState
      ),
      "nativeTargetPresentationActive": nativeTargetActive,
      "nativeTargetRendererInitialized": nativeTargetState["rendererInitialized"] ?? false,
      "nativeTargetInstalled": nativeTargetState["targetInstalled"] ?? false,
      "nativeTargetBackendAvailable": nativeTargetState["backendAvailable"] ?? false,
      "nativeTargetBackendName": nativeTargetState["backendName"] ?? "unknown",
      "nativeTargetLastDrawSucceeded": nativeTargetState["lastDrawSucceeded"] ?? false,
      "nativeTargetConsecutiveDrawFailures":
        nativeTargetState["consecutiveDrawFailures"] ?? 0,
      "nativeTargetDrawFailureCount": nativeTargetState["drawFailureCount"] ?? 0,
      "nativeTargetGeneration": nativeTargetState["targetGeneration"] ?? 0,
      "nativeTargetUploadIntervalP95Ms": nativeTargetState["uploadIntervalP95Ms"] ?? 0,
      "nativeTargetWarmupGeneration":
        nativeTargetState["targetWarmupGeneration"] ?? 0,
      "nativeTargetWarmupRemaining":
        nativeTargetState["targetWarmupRemaining"] ?? 0,
      "nativeTargetWarmupSampleCount":
        nativeTargetState["targetWarmupSampleCount"] ?? 0,
      "nativeTargetWarmupLastMs":
        nativeTargetState["targetWarmupLastMs"] ?? 0,
      "nativeTargetWarmupP95Ms":
        nativeTargetState["targetWarmupP95Ms"] ?? 0,
      "nativeTargetWidth": nativeTargetState["targetWidth"] ?? 0,
      "nativeTargetHeight": nativeTargetState["targetHeight"] ?? 0,
      "nativeTargetUploadStorageKind": nativeTargetState["uploadStorageKind"] ?? "unavailable",
      "nativeTargetLastSuccessfulFramePtsUs":
        nativeTargetState["lastSuccessfulFramePtsUs"] ?? 0,
      "nativeTargetLastFrameColorRangeCode": nativeTargetLastFrameColorRangeCode,
      "nativeTargetLastFrameColorRange": colorRangeName(nativeTargetLastFrameColorRangeCode),
      "nativeTargetLastFrameColorMatrixCode": nativeTargetLastFrameColorMatrixCode,
      "nativeTargetLastFrameColorMatrix":
        colorMatrixName(nativeTargetLastFrameColorMatrixCode),
      "nativeTargetLastFrameColorTransferCode": nativeTargetLastFrameColorTransferCode,
      "nativeTargetLastFrameColorTransfer":
        colorTransferName(nativeTargetLastFrameColorTransferCode),
      "nativeTargetLastFrameColorPrimariesCode": nativeTargetLastFrameColorPrimariesCode,
      "nativeTargetLastFrameColorPrimaries":
        colorPrimariesName(nativeTargetLastFrameColorPrimariesCode),
      "nativeTargetOverlayLastExpected":
        nativeTargetState["overlayLastExpected"] ?? false,
      "nativeTargetOverlayLastApplied":
        nativeTargetState["overlayLastApplied"] ?? false,
      "nativeTargetOverlayLastFillRectCount":
        nativeTargetState["overlayLastFillRectCount"] ?? 0,
      "nativeTargetOverlayLastLineRectCount":
        nativeTargetState["overlayLastLineRectCount"] ?? 0,
      "nativeTargetOverlayExpectedCount":
        nativeTargetState["overlayExpectedCount"] ?? 0,
      "nativeTargetOverlayAppliedCount":
        nativeTargetState["overlayAppliedCount"] ?? 0,
      "nativeTargetOverlayMissedCount":
        nativeTargetState["overlayMissedCount"] ?? 0,
      "nativeTargetOverlayGpuSuccessCount":
        nativeTargetState["overlayGpuSuccessCount"] ?? 0,
      "nativeTargetOverlayGpuFailureCount":
        nativeTargetState["overlayGpuFailureCount"] ?? 0,
      "nativeTargetOverlayCpuFallbackCount":
        nativeTargetState["overlayCpuFallbackCount"] ?? 0,
      "nativeTargetLastDrawError": nativeTargetState["lastDrawError"] ?? "",
      "hardwareDecodeProvider": String(cString: VPMacOSNativeHardwareDecodeProviderName()),
      "hardwareDecodeAvailable": VPMacOSNativeHardwareDecodeAvailable() != 0,
      "hardwareDecodeActive": player?.hardwareDecodeActive() ?? false,
      "hardwareDecodeDownloadsToCpu": player?.hardwareDecodeDownloadsToCpu() ?? false,
      "decodeMode": player?.decodeModeName() ?? "none",
      "nativeTrackDiagnostics": trackDiagnostics,
      "nativeTrackDiagnosticCount": trackDiagnostics.count,
      "primaryTrackFileId": primaryTrack?["fileId"] ?? -1,
      "primaryTrackSlot": primaryTrack?["slot"] ?? -1,
      "primaryTrackDecodeMode": primaryTrack?["decodeMode"] ?? "none",
      "primaryTrackDecoderName": primaryTrack?["decoderName"] ?? "none",
      "softwareFrameStorageKind": primaryTrack?["softwareFrameStorageKind"] ?? "empty",
      "softwareFrameStorageKindCode": primaryTrack?["softwareFrameStorageKindCode"] ?? 0,
      "softwareFrameYuvBitDepth": primaryTrack?["softwareFrameYuvBitDepth"] ?? 0,
      "softwareFramePackFallbackCount":
        primaryTrack?["softwareFramePackFallbackCount"] ?? 0,
      "primaryTrackSoftwareFrameStorageKind":
        primaryTrack?["softwareFrameStorageKind"] ?? "empty",
      "primaryTrackSoftwareFramePackFallbackCount":
        primaryTrack?["softwareFramePackFallbackCount"] ?? 0,
      "primaryTrackHardwareDecodeActive": primaryTrack?["hardwareDecodeActive"] ?? false,
      "primaryTrackFramesDecoded": primaryTrack?["framesDecoded"] ?? 0,
      "primaryTrackDecodeFps": primaryTrack?["decodeFps"] ?? 0.0,
      "primaryTrackDecodeFpsX1000": primaryTrack?["decodeFpsX1000"] ?? 0,
      "primaryTrackDecodeAvgMs": primaryTrack?["decodeAvgMs"] ?? 0.0,
      "primaryTrackDecodeMaxMs": primaryTrack?["decodeMaxMs"] ?? 0.0,
      "primaryTrackDecodeStagePacketSendCount":
        primaryTrack?["decodeStagePacketSendCount"] ?? 0,
      "primaryTrackDecodeStagePacketSendAvgMs":
        primaryTrack?["decodeStagePacketSendAvgMs"] ?? 0.0,
      "primaryTrackDecodeStagePacketSendMaxMs":
        primaryTrack?["decodeStagePacketSendMaxMs"] ?? 0.0,
      "primaryTrackDecodeStageReceiveLoopCount":
        primaryTrack?["decodeStageReceiveLoopCount"] ?? 0,
      "primaryTrackDecodeStageReceiveFrameCount":
        primaryTrack?["decodeStageReceiveFrameCount"] ?? 0,
      "primaryTrackDecodeStageReceiveAvgMs":
        primaryTrack?["decodeStageReceiveAvgMs"] ?? 0.0,
      "primaryTrackDecodeStageReceiveMaxMs":
        primaryTrack?["decodeStageReceiveMaxMs"] ?? 0.0,
      "primaryTrackDecodeStageConvertCount":
        primaryTrack?["decodeStageConvertCount"] ?? 0,
      "primaryTrackDecodeStageConvertAvgMs":
        primaryTrack?["decodeStageConvertAvgMs"] ?? 0.0,
      "primaryTrackDecodeStageConvertMaxMs":
        primaryTrack?["decodeStageConvertMaxMs"] ?? 0.0,
      "primaryTrackDecodeStageConvertDirectPlanarCount":
        primaryTrack?["decodeStageConvertDirectPlanarCount"] ?? 0,
      "primaryTrackDecodeStageConvertDirectPlanarAvgMs":
        primaryTrack?["decodeStageConvertDirectPlanarAvgMs"] ?? 0.0,
      "primaryTrackDecodeStageConvertDirectPlanarMaxMs":
        primaryTrack?["decodeStageConvertDirectPlanarMaxMs"] ?? 0.0,
      "primaryTrackDecodeStageConvertNv12LayoutCount":
        primaryTrack?["decodeStageConvertNv12LayoutCount"] ?? 0,
      "primaryTrackDecodeStageConvertNv12LayoutAvgMs":
        primaryTrack?["decodeStageConvertNv12LayoutAvgMs"] ?? 0.0,
      "primaryTrackDecodeStageConvertNv12LayoutMaxMs":
        primaryTrack?["decodeStageConvertNv12LayoutMaxMs"] ?? 0.0,
      "primaryTrackDecodeStageConvertNv12AllocCount":
        primaryTrack?["decodeStageConvertNv12AllocCount"] ?? 0,
      "primaryTrackDecodeStageConvertNv12AllocAvgMs":
        primaryTrack?["decodeStageConvertNv12AllocAvgMs"] ?? 0.0,
      "primaryTrackDecodeStageConvertNv12AllocMaxMs":
        primaryTrack?["decodeStageConvertNv12AllocMaxMs"] ?? 0.0,
      "primaryTrackDecodeStageConvertNv12PackCount":
        primaryTrack?["decodeStageConvertNv12PackCount"] ?? 0,
      "primaryTrackDecodeStageConvertNv12PackAvgMs":
        primaryTrack?["decodeStageConvertNv12PackAvgMs"] ?? 0.0,
      "primaryTrackDecodeStageConvertNv12PackMaxMs":
        primaryTrack?["decodeStageConvertNv12PackMaxMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishCount":
        primaryTrack?["decodeStagePublishCount"] ?? 0,
      "primaryTrackDecodeStagePublishAvgMs":
        primaryTrack?["decodeStagePublishAvgMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishMaxMs":
        primaryTrack?["decodeStagePublishMaxMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishLockCount":
        primaryTrack?["decodeStagePublishLockCount"] ?? 0,
      "primaryTrackDecodeStagePublishLockAvgMs":
        primaryTrack?["decodeStagePublishLockAvgMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishLockMaxMs":
        primaryTrack?["decodeStagePublishLockMaxMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishWaitCount":
        primaryTrack?["decodeStagePublishWaitCount"] ?? 0,
      "primaryTrackDecodeStagePublishWaitAvgMs":
        primaryTrack?["decodeStagePublishWaitAvgMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishWaitMaxMs":
        primaryTrack?["decodeStagePublishWaitMaxMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishRingPushCount":
        primaryTrack?["decodeStagePublishRingPushCount"] ?? 0,
      "primaryTrackDecodeStagePublishRingPushAvgMs":
        primaryTrack?["decodeStagePublishRingPushAvgMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishRingPushMaxMs":
        primaryTrack?["decodeStagePublishRingPushMaxMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishRingLockCount":
        primaryTrack?["decodeStagePublishRingLockCount"] ?? 0,
      "primaryTrackDecodeStagePublishRingLockAvgMs":
        primaryTrack?["decodeStagePublishRingLockAvgMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishRingLockMaxMs":
        primaryTrack?["decodeStagePublishRingLockMaxMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishRingAssignCount":
        primaryTrack?["decodeStagePublishRingAssignCount"] ?? 0,
      "primaryTrackDecodeStagePublishRingAssignAvgMs":
        primaryTrack?["decodeStagePublishRingAssignAvgMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishRingAssignMaxMs":
        primaryTrack?["decodeStagePublishRingAssignMaxMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishRingAdvanceCount":
        primaryTrack?["decodeStagePublishRingAdvanceCount"] ?? 0,
      "primaryTrackDecodeStagePublishRingAdvanceAvgMs":
        primaryTrack?["decodeStagePublishRingAdvanceAvgMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishRingAdvanceMaxMs":
        primaryTrack?["decodeStagePublishRingAdvanceMaxMs"] ?? 0.0,
      "primaryTrackDecodeStagePublishRingOverwriteCount":
        primaryTrack?["decodeStagePublishRingOverwriteCount"] ?? 0,
      "primaryTrackDecodeStagePublishRingOverwriteAvgBytes":
        primaryTrack?["decodeStagePublishRingOverwriteAvgBytes"] ?? 0.0,
      "primaryTrackDecodeStagePublishRingOverwriteMaxBytes":
        primaryTrack?["decodeStagePublishRingOverwriteMaxBytes"] ?? 0,
      "primaryTrackDecodeStageFlushCount":
        primaryTrack?["decodeStageFlushCount"] ?? 0,
      "primaryTrackDecodeStageFlushAvgMs":
        primaryTrack?["decodeStageFlushAvgMs"] ?? 0.0,
      "primaryTrackDecodeStageFlushMaxMs":
        primaryTrack?["decodeStageFlushMaxMs"] ?? 0.0,
      "primaryTrackBufferCount": primaryTrack?["bufferCount"] ?? 0,
      "primaryTrackBufferCapacity": primaryTrack?["bufferCapacity"] ?? 0,
      "primaryTrackBufferState": primaryTrack?["bufferState"] ?? 0,
      "primaryTrackCurrentPtsUs": primaryTrack?["currentPtsUs"] ?? 0,
      "secondaryTrackFileId": secondaryTrack?["fileId"] ?? -1,
      "secondaryTrackSlot": secondaryTrack?["slot"] ?? -1,
      "secondaryTrackDecodeMode": secondaryTrack?["decodeMode"] ?? "none",
      "secondaryTrackDecoderName": secondaryTrack?["decoderName"] ?? "none",
      "secondaryTrackOffsetUs": secondaryTrack?["offsetUs"] ?? 0,
      "softwareFallbackActive": player?.hardwareDecodeActive() != true,
      "available": player != nil,
      "reason": player == nil
        ? "Explicit macOS synthetic texture source is active"
        : presentationReason(
          player: player,
          state: nativeTargetState
        ),
      "playerId": playerId ?? -1,
      "textureId": textureId ?? -1,
      "flutterVideoTextureRegistered": textureId != nil,
      "textureWidth": textureDimensions?.width ?? 0,
      "textureHeight": textureDimensions?.height ?? 0,
      "trackCount": trackCount,
      "isPlaying": isPlaying,
      "audioAvailable": player?.hasAudio() ?? false,
      "audioSampleRate": player?.audioSampleRate() ?? 0,
      "audioChannels": player?.audioChannels() ?? 0,
      "activeAudioTrack": player?.activeAudioTrack() ?? -1,
      "audioOutputDeviceInitialized":
        audioDiagnostics["audioOutputDeviceInitialized"] ?? false,
      "audioOutputPlaying": audioDiagnostics["audioOutputPlaying"] ?? false,
      "audioOutputActiveTrack": audioDiagnostics["audioOutputActiveTrack"] ?? -1,
      "audioOutputSampleRate": audioDiagnostics["audioOutputSampleRate"] ?? 0,
      "audioOutputChannels": audioDiagnostics["audioOutputChannels"] ?? 0,
      "audioOutputRegisteredTrackCount":
        audioDiagnostics["audioOutputRegisteredTrackCount"] ?? 0,
      "audioOutputActiveTrackRegistered":
        audioDiagnostics["audioOutputActiveTrackRegistered"] ?? false,
      "audioOutputQueuedFrames": audioDiagnostics["audioOutputQueuedFrames"] ?? 0,
      "audioOutputQueuedDurationUs":
        audioDiagnostics["audioOutputQueuedDurationUs"] ?? 0,
      "audioOutputUnderrunFrames":
        audioDiagnostics["audioOutputUnderrunFrames"] ?? 0,
      "audioOutputDiscardedFrames":
        audioDiagnostics["audioOutputDiscardedFrames"] ?? 0,
      "audioOutputSeekTrimmedFrames":
        audioDiagnostics["audioOutputSeekTrimmedFrames"] ?? 0,
      "primaryTrackOffsetUs": player?.trackOffsetUs(fileId: 0) ?? 0,
      "legacySecondaryTrackOffsetUs": player?.trackOffsetUs(fileId: 1) ?? 0,
      "presentationSchedulerTickCount": schedulerStats?["tickCount"] ?? 0,
      "presentationSchedulerPresentableTickCount": schedulerStats?["presentableTickCount"] ?? 0,
      "presentationSchedulerFrameNotificationCount": schedulerStats?["frameNotificationCount"] ?? 0,
      "presentationSchedulerLastSelectedPtsUs": schedulerStats?["lastSelectedPtsUs"] ?? -1,
      "presentationSchedulerLastPresentFrameCount": schedulerStats?["lastPresentFrameCount"] ?? 0,
      "presentationSchedulerCachedDecisionAvailable": schedulerStats?["cachedPresentDecisionAvailable"] ?? false,
      "presentationSchedulerDeadlineSleepCount": schedulerStats?["deadlineSleepCount"] ?? 0,
      "presentationSchedulerLastDeadlineSleepUs": schedulerStats?["lastDeadlineSleepUs"] ?? 0,
      "nativeLayoutMode": layoutSnapshot?["mode"] ?? -1,
      "nativeLayoutZoomRatio": layoutSnapshot?["zoomRatio"] ?? 0.0,
      "nativeLayoutPixelSizeMode": layoutSnapshot?["pixelSizeMode"] ?? -1,
      "pixelBufferRebuildCount": textureStats?.rebuildCount ?? 0,
      "pixelBufferReuseCount": textureStats?.reuseCount ?? 0,
      "pixelBufferAllocationCount": textureStats?.allocationCount ?? 0,
      "pixelBufferRebuildReuseCount": textureStats?.rebuildReuseCount ?? 0,
      "pixelBufferRebuildLastAllocatedCount": textureStats?.rebuildLastAllocatedCount ?? 0,
      "pixelBufferRebuildLastReusedCount": textureStats?.rebuildLastReusedCount ?? 0,
      "pixelBufferRebuildLastDurationMs": textureStats?.rebuildLastDurationMs ?? 0.0,
      "retiredPixelBufferCount": textureStats?.retiredPixelBufferCount ?? 0,
      "retiredPixelBufferBytes": textureStats?.retiredPixelBufferBytes ?? 0,
      "pixelBufferPrewarmRequestCount": textureStats?.prewarmRequestCount ?? 0,
      "pixelBufferPrewarmHitCount": textureStats?.prewarmHitCount ?? 0,
      "pixelBufferPrewarmReadyCount": textureStats?.prewarmReadyCount ?? 0,
      "pixelBufferPrewarmDroppedCount": textureStats?.prewarmDroppedCount ?? 0,
      "pixelBufferMetalUploadCount": textureStats?.metalUploadCount ?? 0,
      "pixelBufferMetalYuvUploadCount":
        perfStats?["nativeTargetDirectYuvUploadCount"] ?? 0,
      "pixelBufferMetalCVPixelBufferUploadCount":
        perfStats?["nativeTargetCVPixelBufferUploadCount"] ?? 0,
      "pixelBufferMetalUploadFailureCount": textureStats?.metalUploadFailureCount ?? 0,
      "presentationUploadMode": MacOSPresentationDiagnostics.uploadMode(
          perfStats: perfStats,
          targetReady: textureStats?.metalTextureValid ?? false,
          targetInstalled: presentationTargetInstalled,
          textureRegistered: textureId != nil
        ),
      "presentationPackageUploadCount":
        perfStats?["nativeTargetPresentPackageUploadCount"] ?? 0,
      "presentationPackageCopyUs": perfStats?["nativeTargetPresentPackageCopyUs"] ?? 0,
      "presentationPackageGpuWaitUs":
        perfStats?["nativeTargetPresentPackageGpuWaitUs"] ?? 0,
      "presentationPackageTotalUs":
        perfStats?["nativeTargetPresentPackageTotalUs"] ?? 0,
      "presentationPackageStorage":
        perfStats?["nativeTargetPresentPackageStorage"] ?? "unavailable",
      "presentationPackageStagingAllocationCount":
        perfStats?["nativeTargetStagingAllocationCount"] ?? 0,
      "presentationPackageStagingReuseCount":
        perfStats?["nativeTargetStagingReuseCount"] ?? 0,
      "presentationPackageStagingMaxBytes":
        perfStats?["nativeTargetStagingMaxBytes"] ?? 0,
      "metalAvailable": textureStats?.metalAvailable ?? false,
      "metalTextureCacheAvailable": textureStats?.metalTextureCacheAvailable ?? false,
      "metalTextureValid": textureStats?.metalTextureValid ?? false,
      "metalTextureCreationCount": textureStats?.metalTextureCreationCount ?? 0,
      "metalTextureFailureCount": textureStats?.metalTextureFailureCount ?? 0,
      "metalTextureLastError": textureStats?.metalTextureLastError ?? "",
      "textureInFlightMetalBufferCount": textureStats?.inFlightMetalBufferCount ?? 0,
      "textureMetalBufferExhaustionCount": textureStats?.metalBufferExhaustionCount ?? 0,
      "nativeStableDisplayFallbackActive":
        textureStats?.stableDisplayFallbackActive ?? false,
      "nativeStableDisplayFallbackCount": textureStats?.stableDisplayFallbackCount ?? 0,
      "nativeStableDisplayFallbackPtsUs": textureStats?.stableDisplayFallbackPtsUs ?? -1,
      "nativePresentationTargetInstalled": presentationTargetInstalled,
      "nativeTargetUploadCount": player?.nativeTargetPresentationUploadCount() ?? 0,
      "nativeTargetUploadFailureCount": player?.nativeTargetPresentationFailureCount() ?? 0,
      "nativeTargetUploadFps": perfStats?["nativeTargetUploadFps"] ?? 0.0,
      "nativeTargetUploadFpsX1000": perfStats?["nativeTargetUploadFpsX1000"] ?? 0,
      "nativeTargetUploadElapsedMs": perfStats?["nativeTargetUploadElapsedMs"] ?? 0,
      "nativeRendererDrawCount": rendererDrawCount,
      "nativeRendererDrawAvgUs": perfStats?["rendererDrawAvgUs"] ?? 0,
      "nativeRendererDrawMaxUs": perfStats?["rendererDrawMaxUs"] ?? 0,
      "nativeRendererDrawP95Us": perfStats?["rendererDrawP95Us"] ?? 0,
      "nativeRendererDrawBackendAvgUs": perfStats?["rendererDrawBackendAvgUs"] ?? 0,
      "nativeRendererDrawBackendMaxUs": perfStats?["rendererDrawBackendMaxUs"] ?? 0,
      "nativeRendererDrawBackendP95Us": perfStats?["rendererDrawBackendP95Us"] ?? 0,
      "layoutPresentedCount": perfStats?["rendererLayoutPresentedCount"] ?? 0,
      "layoutDeferredToPlaybackCount":
        perfStats?["rendererLayoutDeferredToPlaybackCount"] ?? 0,
      "playingLayoutRedrawSuppressedCount":
        perfStats?["rendererPlayingLayoutRedrawSuppressedCount"] ?? 0,
      "rendererPlayingLayoutRedrawSuppressedCount":
        perfStats?["rendererPlayingLayoutRedrawSuppressedCount"] ?? 0,
      "layoutStaleCompletionDropCount":
        perfStats?["rendererLayoutStaleCompletionDropCount"] ?? 0,
      "rendererLastLayoutRevision": perfStats?["rendererLastLayoutRevision"] ?? 0,
      "rendererLastPresentedLayoutRevision":
        perfStats?["rendererLastPresentedLayoutRevision"] ?? 0,
      "drawsPerPresentedFrameRatioX1000": drawsPerPresentedFrameRatioX1000,
      "inFlightMetalBufferCount": max(
        nativeInFlightMetalBufferCount,
        textureInFlightMetalBufferCount
      ),
      "metalBufferExhaustionCount": max(
        nativeMetalBufferExhaustionCount,
        textureMetalBufferExhaustionCount
      ),
      "metalCommandCompletionP95Us": perfStats?["metalCommandCompletionP95Us"] ?? 0,
      "metalCommandFailureCount": perfStats?["metalCommandFailureCount"] ?? 0,
      "asyncMetalPublishActive": perfStats?["asyncMetalPublishActive"] ?? false,
      "videoSourceUpdateCount": perfStats?["videoSourceUpdateCount"] ?? 0,
      "viewportCompositeCount": perfStats?["viewportCompositeCount"] ?? 0,
      "sourceFrameCacheHitCount": perfStats?["sourceFrameCacheHitCount"] ?? 0,
      "sourceFrameCacheMissCount": perfStats?["sourceFrameCacheMissCount"] ?? 0,
      "sourceFrameCacheHitRatioX1000":
        perfStats?["sourceFrameCacheHitRatioX1000"] ?? 0,
      "processRssBytes": perfStats?["processRssBytes"] ?? 0,
      "processPrivateBytes": perfStats?["processPrivateBytes"] ?? 0,
      "dedicatedGpuUsageBytes": dedicatedGpuUsageBytes,
      "nativeDedicatedGpuUsageBytes": nativeDedicatedGpuUsageBytes,
      "nativeTargetPixelBufferBytes": nativeTargetPixelBufferBytes,
      "nativeTargetPixelBufferCount": textureStats?.nativeTargetPixelBufferCount ?? 0,
      "nativeDecodeFrameCount": perfStats?["decodeFrameCount"] ?? 0,
      "nativeDecodeDroppedCount": perfStats?["decodeDroppedCount"] ?? 0,
      "nativeDecodeElapsedMs": perfStats?["decodeElapsedMs"] ?? 0,
      "nativeDecodeFps": perfStats?["decodeFps"] ?? 0.0,
      "nativeDecodeFpsX1000": perfStats?["decodeFpsX1000"] ?? 0,
      "nativeDecodeAvgMs": perfStats?["decodeAvgMs"] ?? 0.0,
      "nativeDecodeMaxMs": perfStats?["decodeMaxMs"] ?? 0.0,
      "nativeActiveTrackCount": perfStats?["activeTrackCount"] ?? 0,
      "nativeAggregateDecodeFrameCount": perfStats?["aggregateDecodeFrameCount"] ?? 0,
      "nativeAggregateDecodeFps": perfStats?["aggregateDecodeFps"] ?? 0.0,
      "nativeAggregateDecodeFpsX1000": perfStats?["aggregateDecodeFpsX1000"] ?? 0,
      "cpuFrameMemoryBytes": perfStats?["cpuFrameMemoryBytes"] ?? 0,
      "packetQueueMemoryBytes": perfStats?["packetQueueMemoryBytes"] ?? 0,
      "nativeCpuFrameMemoryBytes": perfStats?["cpuFrameMemoryBytes"] ?? 0,
      "nativePacketQueueMemoryBytes": perfStats?["packetQueueMemoryBytes"] ?? 0,
      "presentationFallbackReason": MacOSPresentationDiagnostics.fallbackReason(
          player: player,
          targetInstalled: presentationTargetInstalled,
          perfStats: perfStats
        ),
    ]
    nativeEventDiagnostics.forEach { diagnostics[$0.key] = $0.value }
    frameCallbackDiagnostics.forEach { diagnostics[$0.key] = $0.value }
    viewportDiagnostics.forEach { diagnostics[$0.key] = $0.value }
    presentationDiagnostics.forEach { diagnostics[$0.key] = $0.value }
    return diagnostics
  }

  private static func presentationBackendName(
    player: MacOSNativePlayerSession?,
    state: [String: Any]
  ) -> String {
    guard player != nil else {
      return "explicit-synthetic-texture"
    }
    if state["active"] as? Bool == true {
      return "native-metal-cvpixelbuffer-target"
    }
    return "native-metal-target-unavailable"
  }

  private static func presentationAdapterKind(
    player: MacOSNativePlayerSession?,
    state: [String: Any]
  ) -> String {
    guard player != nil else {
      return "explicit-synthetic"
    }
    if state["active"] as? Bool == true {
      return "native-target-metal"
    }
    return "unavailable"
  }

  private static func presentationReason(
    player: MacOSNativePlayerSession?,
    state: [String: Any]
  ) -> String {
    if player != nil && state["active"] as? Bool == true {
      return "macOS shared renderer is active with native Metal presentation"
    }
    if let error = state["lastDrawError"] as? String, !error.isEmpty {
      return error
    }
    return "macOS shared renderer has no active native target presentation target"
  }

  private static func emptyNativeTargetPresentationState() -> [String: Any] {
    [
      "rendererInitialized": false,
      "targetInstalled": false,
      "backendAvailable": false,
      "backendName": "unknown",
      "active": false,
      "lastDrawSucceeded": false,
      "consecutiveDrawFailures": 0,
      "drawFailureCount": 0,
      "targetGeneration": 0,
      "uploadIntervalP95Ms": 0,
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
      "lastDrawError": "",
    ]
  }

  private static func int64Diagnostic(_ value: Any?) -> Int64 {
    if let value = value as? Int64 {
      return value
    }
    if let value = value as? Int {
      return Int64(value)
    }
    if let value = value as? UInt64 {
      return Int64(min(value, UInt64(Int64.max)))
    }
    if let value = value as? Double {
      return Int64(value)
    }
    return 0
  }

  private static func intDiagnostic(_ value: Any?) -> Int {
    if let value = value as? Int {
      return value
    }
    if let value = value as? Int32 {
      return Int(value)
    }
    if let value = value as? Int64 {
      return Int(value)
    }
    if let value = value as? UInt64 {
      return Int(min(value, UInt64(Int.max)))
    }
    if let value = value as? Double {
      return Int(value)
    }
    return 0
  }

  private static func colorDiagnosticCode(
    state: [String: Any],
    stateKey: String,
    fallbackTrack: [String: Any]?,
    trackKey: String
  ) -> Int {
    let stateValue = intDiagnostic(state[stateKey])
    if stateValue != 0 {
      return stateValue
    }
    return MacOSFlutterArguments.intValue(fallbackTrack?[trackKey]) ?? 0
  }

  private static func colorRangeName(_ value: Int) -> String {
    switch value {
    case 1:
      return "limited"
    case 2:
      return "full"
    default:
      return "unknown"
    }
  }

  private static func colorMatrixName(_ value: Int) -> String {
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

  private static func colorTransferName(_ value: Int) -> String {
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

  private static func colorPrimariesName(_ value: Int) -> String {
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
}
