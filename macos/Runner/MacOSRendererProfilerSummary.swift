import Foundation

enum MacOSRendererProfilerSummary {
  static func log(
    isPlaying: Bool,
    trackCount: Int,
    viewport: [String: Any],
    callbacks: [String: Any],
    presentationFrames: [String: Any],
    perf: [String: Any],
    texture: [String: Any],
    scheduler: [String: Any],
    compositor: [String: Any],
    sourceRing: [String: Any]
  ) {
    let presentedCount = int64Value(presentationFrames, "nativeFramePresentationCount")
    let drawCount = int64Value(perf, "rendererDrawCount")
    let drawsPerPresentedFrameX1000 = presentedCount > 0
      ? drawCount * 1000 / presentedCount
      : 0
    let summary = String(
      format: "playing=%d tracks=%d clock=%@ refreshHz=%.1f displayTickHz=%.1f deliveredTickHz=%.1f layoutIntentHz=%.1f layoutSubmitHz=%.1f layoutDrawHz=%.1f layoutSkipHz=%.1f layoutDeferred=%lld layoutPublished=%lld layoutStaleAfterDraw=%lld layoutSuperseded=%lld layoutCallbackSuppressed=%lld nativeLayoutPresented=%lld drawPerFrameX1000=%lld layoutTotalP95Ms=%.2f layoutTotalLastMs=%.2f frameAvailableHz=%.1f callbackQueuedHz=%.1f callbackProcessedHz=%.1f callbackCoalescedHz=%.1f callbackWaitLastMs=%.2f callbackHandleLastMs=%.2f sourceRingLive=%d sourceRingDepth=%lld sourceRingTracks=%lld sourceRingReq=%lld@%.1fHz sourceRingQueueP95Ms=%.2f sourceRingQueueLastMs=%.2f sourceRingCoalesced=%lld sourceRingBaking=%d sourceRingPending=%d sourceRingBake=%lld@%.1fHz sourceRingBakeP95Ms=%.2f sourceRingBakeLastMs=%.2f sourceRingDrawnLast=%lld sourceRingPublish=%lld@%.1fHz sourceRingReqToPubP95Ms=%.2f sourceRingReqToPubLastMs=%.2f sourceRingMiss=%lld sourceRingPtsUs=%lld sourceRingPtsStepP95Ms=%.2f sourceRingPtsStepLastMs=%.2f sourceRingError=%@ presentedCount=%lld duplicatePts=%lld largeGap=%lld rendererRatioX1000=%lld drawCount=%lld drawAvgUs=%lld drawP95Us=%lld drawBackendAvgUs=%lld drawBackendP95Us=%lld uploadFps=%.1f schedulerTicks=%lld presentableTicks=%lld lastPtsUs=%lld videoSourceUpdates=%lld viewportComposites=%lld sourceCacheHits=%lld sourceCacheMisses=%lld sourceCacheHitRatioX1000=%lld targetRebuild=%lld targetAlloc=%lld targetRebuildReuse=%lld targetLastAlloc=%lld targetLastReuse=%lld targetLastMs=%.2f retiredBuffers=%lld prewarm=%lld/%lld/%lld/%lld inFlightMetal=%lld metalBufferExhaustion=%lld asyncMetal=%d metalCompletionP95Us=%lld metalFailures=%lld staleCompletionDrops=%lld compositorTraceHz=%.1f compositorTraceReceived=%lld compositorTraceComposited=%lld compositorTraceCoalesced=%lld compositorDartToSwiftP95Ms=%.2f compositorSwiftQueueP95Ms=%.2f compositorReceiveToCompositeP95Ms=%.2f compositorBackend=%@ compositorHz=%.1f compositorTickHz=%.1f compositorTickP95Ms=%.2f compositorFrameP95Ms=%.2f videoAcquireP95Ms=%.2f flutterAcquireP95Ms=%.2f sourceAcquireP95Ms=%.2f drawableAcquireP95Ms=%.2f backendSubmitP95Ms=%.2f backendCompletionP95Ms=%.2f inFlightSkipHz=%.1f staticSkipHz=%.1f sourceChangeHz=%.1f videoChangeHz=%.1f flutterChangeHz=%.1f",
      isPlaying ? 1 : 0,
      trackCount,
      stringValue(viewport, "viewportClockSource", defaultValue: "unknown"),
      doubleValue(viewport, "displayRefreshHzEstimate"),
      doubleValue(viewport, "displayTickHz"),
      doubleValue(viewport, "displayDeliveredTickHz"),
      doubleValue(viewport, "layoutIntentHz"),
      doubleValue(viewport, "layoutSubmitHz"),
      doubleValue(viewport, "layoutDrawHz"),
      doubleValue(viewport, "layoutSkipHz"),
      int64Value(viewport, "viewportLayoutDeferredToPlaybackCount"),
      int64Value(viewport, "layoutPublishedCount"),
      int64Value(viewport, "layoutStaleAfterDrawDropCount"),
      int64Value(viewport, "layoutRefreshSupersededCount"),
      int64Value(viewport, "layoutCallbackPublicationSuppressedCount"),
      int64Value(perf, "rendererLayoutPresentedCount"),
      drawsPerPresentedFrameX1000,
      doubleValue(viewport, "layoutRefreshTotalP95Ms"),
      doubleValue(viewport, "layoutRefreshTotalLastMs"),
      doubleValue(callbacks, "frameAvailableHz"),
      doubleValue(callbacks, "macosFrameCallbackQueuedHz"),
      doubleValue(callbacks, "macosFrameCallbackProcessedHz"),
      doubleValue(callbacks, "macosFrameCallbackCoalescedHz"),
      doubleValue(callbacks, "macosFrameCallbackMainWaitLastMs"),
      doubleValue(callbacks, "macosFrameCallbackHandleLastMs"),
      boolValue(sourceRing, "sourceRingLive") ? 1 : 0,
      int64Value(sourceRing, "sourceRingDepth"),
      int64Value(sourceRing, "sourceRingTrackCount"),
      int64Value(sourceRing, "sourceRingRefreshRequestCount"),
      doubleValue(sourceRing, "sourceRingRefreshRequestHz"),
      doubleValue(sourceRing, "sourceRingRefreshQueueWaitP95Ms"),
      doubleValue(sourceRing, "sourceRingRefreshQueueWaitLastMs"),
      int64Value(sourceRing, "sourceRingRefreshCoalescedCount"),
      boolValue(sourceRing, "sourceRingBaking") ? 1 : 0,
      boolValue(sourceRing, "sourceRingPending") ? 1 : 0,
      int64Value(sourceRing, "sourceRingBakeCount"),
      doubleValue(sourceRing, "sourceRingBakeHz"),
      doubleValue(sourceRing, "sourceRingBakeP95Ms"),
      doubleValue(sourceRing, "sourceRingBakeLastMs"),
      int64Value(sourceRing, "sourceRingLastBakeDrawnCount"),
      int64Value(sourceRing, "sourceRingPublishCount"),
      doubleValue(sourceRing, "sourceRingPublishHz"),
      doubleValue(sourceRing, "sourceRingRequestToPublishP95Ms"),
      doubleValue(sourceRing, "sourceRingRequestToPublishLastMs"),
      int64Value(sourceRing, "sourceRingPublishMissCount"),
      int64Value(sourceRing, "sourceRingLastPublishedPtsUs"),
      doubleValue(sourceRing, "sourceRingPublishedPtsStepP95Ms"),
      doubleValue(sourceRing, "sourceRingPublishedPtsStepLastMs"),
      stringValue(sourceRing, "sourceRingLastBakeError", defaultValue: ""),
      presentedCount,
      int64Value(presentationFrames, "presentedFramePtsDuplicateCount"),
      int64Value(presentationFrames, "presentedFramePtsLargeGapCount"),
      int64Value(presentationFrames, "nativeFrameRendererOwnedRatioX1000"),
      drawCount,
      int64Value(perf, "rendererDrawAvgUs"),
      int64Value(perf, "rendererDrawP95Us"),
      int64Value(perf, "rendererDrawBackendAvgUs"),
      int64Value(perf, "rendererDrawBackendP95Us"),
      doubleValue(perf, "rendererOwnedUploadFps"),
      int64Value(scheduler, "tickCount"),
      int64Value(scheduler, "presentableTickCount"),
      int64Value(scheduler, "lastSelectedPtsUs"),
      int64Value(perf, "videoSourceUpdateCount"),
      int64Value(perf, "viewportCompositeCount"),
      int64Value(perf, "sourceFrameCacheHitCount"),
      int64Value(perf, "sourceFrameCacheMissCount"),
      int64Value(perf, "sourceFrameCacheHitRatioX1000"),
      int64Value(texture, "pixelBufferRebuildCount"),
      int64Value(texture, "pixelBufferAllocationCount"),
      int64Value(texture, "pixelBufferRebuildReuseCount"),
      int64Value(texture, "pixelBufferRebuildLastAllocatedCount"),
      int64Value(texture, "pixelBufferRebuildLastReusedCount"),
      doubleValue(texture, "pixelBufferRebuildLastDurationMs"),
      int64Value(texture, "retiredPixelBufferCount"),
      int64Value(texture, "pixelBufferPrewarmRequestCount"),
      int64Value(texture, "pixelBufferPrewarmReadyCount"),
      int64Value(texture, "pixelBufferPrewarmHitCount"),
      int64Value(texture, "pixelBufferPrewarmDroppedCount"),
      int64Value(perf, "inFlightMetalBufferCount"),
      int64Value(perf, "metalBufferExhaustionCount"),
      boolValue(perf, "asyncMetalPublishActive") ? 1 : 0,
      int64Value(perf, "metalCommandCompletionP95Us"),
      int64Value(perf, "metalCommandFailureCount"),
      int64Value(perf, "rendererLayoutStaleCompletionDropCount"),
      doubleValue(compositor, "nativeCompositorTraceHz"),
      int64Value(compositor, "nativeCompositorTraceReceivedCount"),
      int64Value(compositor, "nativeCompositorTraceCompositedCount"),
      int64Value(compositor, "nativeCompositorTraceCoalescedBeforeCompositeCount"),
      doubleValue(compositor, "nativeCompositorDartToSwiftP95Ms"),
      doubleValue(compositor, "nativeCompositorSwiftQueueP95Ms"),
      doubleValue(compositor, "nativeCompositorReceiveToCompositeP95Ms"),
      stringValue(compositor, "nativeCompositorBackend", defaultValue: "unknown"),
      doubleValue(compositor, "nativeCompositorCompositeHz"),
      doubleValue(compositor, "nativeCompositorDisplayTickHz"),
      doubleValue(compositor, "nativeCompositorDisplayTickIntervalP95Ms"),
      doubleValue(compositor, "nativeCompositorFrameCpuP95Ms"),
      doubleValue(compositor, "nativeCompositorVideoAcquireP95Ms"),
      doubleValue(compositor, "nativeCompositorFlutterAcquireP95Ms"),
      doubleValue(compositor, "nativeCompositorSourceAcquireP95Ms"),
      doubleValue(compositor, "nativeCompositorDrawableAcquireP95Ms"),
      doubleValue(compositor, "nativeCompositorBackendSubmitCpuP95Ms"),
      doubleValue(compositor, "nativeCompositorBackendCompletionP95Ms"),
      doubleValue(compositor, "nativeCompositorInFlightSkipHz"),
      doubleValue(compositor, "nativeCompositorStaticSkipHz"),
      doubleValue(compositor, "nativeCompositorSourceChangeHz"),
      doubleValue(compositor, "nativeCompositorVideoSourceChangeHz"),
      doubleValue(compositor, "nativeCompositorFlutterSourceChangeHz")
    )
    summary.withCString { pointer in
      VPMacOSLogProfilerSummary(pointer)
    }
    let readySummary = String(
      format: "readyState readyVideoP95Ms=%.2f readySourceP95Ms=%.2f producerVideoHz=%.1f producerSourceHz=%.1f reuseVideo=%lld reuseSource=%lld blockedProducer=%lld",
      doubleValue(compositor, "readyVideoAcquireP95Ms"),
      doubleValue(compositor, "readySourceAcquireP95Ms"),
      doubleValue(compositor, "producerVideoPublishHz"),
      doubleValue(compositor, "producerSourcePublishHz"),
      int64Value(compositor, "displayTickReuseVideoCount"),
      int64Value(compositor, "displayTickReuseSourceCount"),
      int64Value(compositor, "displayTickBlockedProducerCount")
    )
    readySummary.withCString { pointer in
      VPMacOSLogProfilerSummary(pointer)
    }
    let decodeSummary =
      "DecodeStage decodeAvgMs=\(doubleValue(perf, "decodeAvgMs")) " +
      "decodeMaxMs=\(doubleValue(perf, "decodeMaxMs")) " +
      "recvAvgMs=\(doubleValue(perf, "decodeStageReceiveAvgMs")) " +
      "recvMaxMs=\(doubleValue(perf, "decodeStageReceiveMaxMs")) " +
      "convertAvgMs=\(doubleValue(perf, "decodeStageConvertAvgMs")) " +
      "convertMaxMs=\(doubleValue(perf, "decodeStageConvertMaxMs")) " +
      "softwareFrameStorage=\(stringValue(perf, "softwareFrameStorageKind", defaultValue: "empty")) " +
      "softwareFramePackFallback=\(int64Value(perf, "softwareFramePackFallbackCount")) " +
      "convertNv12PackAvgMs=\(doubleValue(perf, "decodeStageConvertNv12PackAvgMs")) " +
      "convertNv12PackMaxMs=\(doubleValue(perf, "decodeStageConvertNv12PackMaxMs")) " +
      "publishAvgMs=\(doubleValue(perf, "decodeStagePublishAvgMs")) " +
      "publishMaxMs=\(doubleValue(perf, "decodeStagePublishMaxMs")) " +
      "publishWaitAvgMs=\(doubleValue(perf, "decodeStagePublishWaitAvgMs")) " +
      "publishWaitMaxMs=\(doubleValue(perf, "decodeStagePublishWaitMaxMs")) " +
      "publishRingPushAvgMs=\(doubleValue(perf, "decodeStagePublishRingPushAvgMs")) " +
      "publishRingAssignAvgMs=\(doubleValue(perf, "decodeStagePublishRingAssignAvgMs")) " +
      "publishRingOverwriteAvgBytes=\(doubleValue(perf, "decodeStagePublishRingOverwriteAvgBytes"))"
    decodeSummary.withCString { pointer in
      VPMacOSLogProfilerSummary(pointer)
    }
  }

  private static func doubleValue(
    _ values: [String: Any],
    _ key: String,
    defaultValue: Double = 0.0
  ) -> Double {
    switch values[key] {
    case let value as Double:
      return value
    case let value as Float:
      return Double(value)
    case let value as Int:
      return Double(value)
    case let value as Int64:
      return Double(value)
    case let value as UInt64:
      return Double(value)
    default:
      return defaultValue
    }
  }

  private static func int64Value(
    _ values: [String: Any],
    _ key: String,
    defaultValue: Int64 = 0
  ) -> Int64 {
    switch values[key] {
    case let value as Int64:
      return value
    case let value as Int:
      return Int64(value)
    case let value as UInt64:
      return Int64(min(value, UInt64(Int64.max)))
    case let value as Double:
      return Int64(value)
    default:
      return defaultValue
    }
  }

  private static func boolValue(_ values: [String: Any], _ key: String) -> Bool {
    switch values[key] {
    case let value as Bool:
      return value
    case let value as Int:
      return value != 0
    case let value as Int64:
      return value != 0
    default:
      return false
    }
  }

  private static func stringValue(
    _ values: [String: Any],
    _ key: String,
    defaultValue: String = ""
  ) -> String {
    values[key] as? String ?? defaultValue
  }
}
