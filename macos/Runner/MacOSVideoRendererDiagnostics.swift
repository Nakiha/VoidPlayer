import Foundation

enum MacOSVideoRendererDiagnostics {
  static func map(
    backendName: String,
    player: MacOSNativePlayerSession?,
    textureId: Int64?,
    textureStats: (
      rebuildCount: Int,
      reuseCount: Int,
      metalUploadCount: Int,
      metalUploadFailureCount: Int,
      metalAvailable: Bool,
      metalTextureCacheAvailable: Bool,
      metalTextureValid: Bool,
      metalTextureCreationCount: Int,
      metalTextureFailureCount: Int,
      metalTextureLastError: String
    )?,
    textureDimensions: (width: Int, height: Int)?,
    trackCount: Int,
    presentationTargetInstalled: Bool,
    nativeEventDiagnostics: [String: Any],
    presentationDiagnostics: [String: Any]
  ) -> [String: Any] {
    let layoutSnapshot = player?.layoutSnapshotMap()
    let schedulerStats = player?.presentationSchedulerStats()
    let perfStats = player?.performanceStats()
    let audioDiagnostics = player?.audioDiagnostics() ?? [:]
    let trackDiagnostics = player?.trackDiagnostics() ?? []
    let primaryTrack = trackDiagnostics.first
    let secondaryTrack = trackDiagnostics.dropFirst().first
    let rendererOwnedState = player?.rendererOwnedPresentationState()
      ?? Self.emptyRendererOwnedPresentationState()
    let rendererOwnedActive = rendererOwnedState["active"] as? Bool ?? false
    var diagnostics: [String: Any] = [
      "platform": "macos",
      "backend": backendName,
      "presentationAdapter": String(cString: VPMacOSNativePresentationAdapterName()),
      "presentationAdapterKind": presentationAdapterKind(
        player: player,
        state: rendererOwnedState
      ),
      "presentationScheduler": String(cString: VPMacOSNativePresentationSchedulerName()),
      "presentationBackend": presentationBackendName(player: player, state: rendererOwnedState),
      "rendererOwnedPresentationActive": rendererOwnedActive,
      "rendererOwnedRendererInitialized": rendererOwnedState["rendererInitialized"] ?? false,
      "rendererOwnedTargetInstalled": rendererOwnedState["targetInstalled"] ?? false,
      "rendererOwnedBackendAvailable": rendererOwnedState["backendAvailable"] ?? false,
      "rendererOwnedLastDrawSucceeded": rendererOwnedState["lastDrawSucceeded"] ?? false,
      "rendererOwnedConsecutiveDrawFailures":
        rendererOwnedState["consecutiveDrawFailures"] ?? 0,
      "rendererOwnedDrawFailureCount": rendererOwnedState["drawFailureCount"] ?? 0,
      "rendererOwnedTargetGeneration": rendererOwnedState["targetGeneration"] ?? 0,
      "rendererOwnedTargetWidth": rendererOwnedState["targetWidth"] ?? 0,
      "rendererOwnedTargetHeight": rendererOwnedState["targetHeight"] ?? 0,
      "rendererOwnedUploadStorageKind": rendererOwnedState["uploadStorageKind"] ?? "unavailable",
      "rendererOwnedLastSuccessfulFramePtsUs":
        rendererOwnedState["lastSuccessfulFramePtsUs"] ?? 0,
      "rendererOwnedLastDrawError": rendererOwnedState["lastDrawError"] ?? "",
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
      "primaryTrackHardwareDecodeActive": primaryTrack?["hardwareDecodeActive"] ?? false,
      "primaryTrackFramesDecoded": primaryTrack?["framesDecoded"] ?? 0,
      "secondaryTrackFileId": secondaryTrack?["fileId"] ?? -1,
      "secondaryTrackSlot": secondaryTrack?["slot"] ?? -1,
      "secondaryTrackDecodeMode": secondaryTrack?["decodeMode"] ?? "none",
      "secondaryTrackDecoderName": secondaryTrack?["decoderName"] ?? "none",
      "secondaryTrackOffsetUs": secondaryTrack?["offsetUs"] ?? 0,
      "softwareFallbackActive": player?.hardwareDecodeActive() != true,
      "available": player != nil,
      "reason": player == nil
        ? "Explicit macOS synthetic texture source is active"
        : presentationReason(player: player, state: rendererOwnedState),
      "textureId": textureId ?? -1,
      "textureWidth": textureDimensions?.width ?? 0,
      "textureHeight": textureDimensions?.height ?? 0,
      "trackCount": trackCount,
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
      "pixelBufferMetalUploadCount": textureStats?.metalUploadCount ?? 0,
      "pixelBufferMetalYuvUploadCount":
        perfStats?["rendererOwnedDirectYuvUploadCount"] ?? 0,
      "pixelBufferMetalCVPixelBufferUploadCount":
        perfStats?["rendererOwnedCVPixelBufferUploadCount"] ?? 0,
      "pixelBufferMetalUploadFailureCount": textureStats?.metalUploadFailureCount ?? 0,
      "presentationUploadMode": MacOSPresentationDiagnostics.uploadMode(
        perfStats: perfStats,
        targetReady: textureStats?.metalTextureValid ?? false,
        targetInstalled: presentationTargetInstalled,
        textureRegistered: textureId != nil
      ),
      "presentationPackageUploadCount":
        perfStats?["rendererOwnedPresentPackageUploadCount"] ?? 0,
      "presentationPackageCopyUs": perfStats?["rendererOwnedPresentPackageCopyUs"] ?? 0,
      "presentationPackageGpuWaitUs":
        perfStats?["rendererOwnedPresentPackageGpuWaitUs"] ?? 0,
      "presentationPackageTotalUs":
        perfStats?["rendererOwnedPresentPackageTotalUs"] ?? 0,
      "presentationPackageStorage":
        perfStats?["rendererOwnedPresentPackageStorage"] ?? "unavailable",
      "presentationPackageStagingAllocationCount":
        perfStats?["rendererOwnedStagingAllocationCount"] ?? 0,
      "presentationPackageStagingReuseCount":
        perfStats?["rendererOwnedStagingReuseCount"] ?? 0,
      "presentationPackageStagingMaxBytes":
        perfStats?["rendererOwnedStagingMaxBytes"] ?? 0,
      "metalAvailable": textureStats?.metalAvailable ?? false,
      "metalTextureCacheAvailable": textureStats?.metalTextureCacheAvailable ?? false,
      "metalTextureValid": textureStats?.metalTextureValid ?? false,
      "metalTextureCreationCount": textureStats?.metalTextureCreationCount ?? 0,
      "metalTextureFailureCount": textureStats?.metalTextureFailureCount ?? 0,
      "metalTextureLastError": textureStats?.metalTextureLastError ?? "",
      "nativePresentationTargetInstalled": presentationTargetInstalled,
      "nativeRendererOwnedUploadCount": player?.rendererOwnedPresentationUploadCount() ?? 0,
      "nativeRendererOwnedUploadFailureCount": player?.rendererOwnedPresentationFailureCount() ?? 0,
      "nativeRendererOwnedUploadFps": perfStats?["rendererOwnedUploadFps"] ?? 0.0,
      "nativeRendererOwnedUploadFpsX1000": perfStats?["rendererOwnedUploadFpsX1000"] ?? 0,
      "nativeRendererOwnedUploadElapsedMs": perfStats?["rendererOwnedUploadElapsedMs"] ?? 0,
      "processRssBytes": perfStats?["processRssBytes"] ?? 0,
      "processPrivateBytes": perfStats?["processPrivateBytes"] ?? 0,
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
      "nativeCpuFrameMemoryBytes": perfStats?["cpuFrameMemoryBytes"] ?? 0,
      "nativePacketQueueMemoryBytes": perfStats?["packetQueueMemoryBytes"] ?? 0,
      "presentationFallbackReason": MacOSPresentationDiagnostics.fallbackReason(
        player: player,
        targetInstalled: presentationTargetInstalled,
        perfStats: perfStats
      ),
    ]
    nativeEventDiagnostics.forEach { diagnostics[$0.key] = $0.value }
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
      return "renderer-owned-metal"
    }
    return "unavailable"
  }

  private static func presentationReason(
    player: MacOSNativePlayerSession?,
    state: [String: Any]
  ) -> String {
    if player != nil && state["active"] as? Bool == true {
      return "macOS shared renderer is active with renderer-owned Metal presentation"
    }
    if let error = state["lastDrawError"] as? String, !error.isEmpty {
      return error
    }
    return "macOS shared renderer has no active renderer-owned presentation target"
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
      "targetGeneration": 0,
      "targetWidth": 0,
      "targetHeight": 0,
      "uploadStorageKind": "unavailable",
      "lastSuccessfulFramePtsUs": 0,
      "lastDrawError": "",
    ]
  }
}
