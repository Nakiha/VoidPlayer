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
    compositor: [String: Any]
  ) {
    let presentedCount = int64Value(presentationFrames, "nativeFramePresentationCount")
    let drawCount = int64Value(perf, "rendererDrawCount")
    let drawsPerPresentedFrameX1000 = presentedCount > 0
      ? drawCount * 1000 / presentedCount
      : 0
    let summary = String(
      format: "playing=%d tracks=%d clock=%@ refreshHz=%.1f displayTickHz=%.1f deliveredTickHz=%.1f layoutIntentHz=%.1f layoutSubmitHz=%.1f layoutDrawHz=%.1f layoutSkipHz=%.1f layoutDeferred=%lld layoutPublished=%lld layoutStaleAfterDraw=%lld layoutSuperseded=%lld layoutCallbackSuppressed=%lld nativeLayoutPresented=%lld drawPerFrameX1000=%lld layoutTotalP95Ms=%.2f layoutTotalLastMs=%.2f frameAvailableHz=%.1f callbackQueuedHz=%.1f callbackProcessedHz=%.1f callbackCoalescedHz=%.1f callbackWaitLastMs=%.2f callbackHandleLastMs=%.2f presentedCount=%lld duplicatePts=%lld largeGap=%lld rendererRatioX1000=%lld drawCount=%lld drawAvgUs=%lld drawP95Us=%lld drawBackendAvgUs=%lld drawBackendP95Us=%lld uploadFps=%.1f schedulerTicks=%lld presentableTicks=%lld lastPtsUs=%lld videoSourceUpdates=%lld viewportComposites=%lld sourceCacheHits=%lld sourceCacheMisses=%lld sourceCacheHitRatioX1000=%lld targetRebuild=%lld targetAlloc=%lld targetRebuildReuse=%lld targetLastAlloc=%lld targetLastReuse=%lld targetLastMs=%.2f retiredBuffers=%lld prewarm=%lld/%lld/%lld/%lld inFlightMetal=%lld metalBufferExhaustion=%lld asyncMetal=%d metalCompletionP95Us=%lld metalFailures=%lld staleCompletionDrops=%lld compositorTraceHz=%.1f compositorTraceReceived=%lld compositorTraceComposited=%lld compositorTraceCoalesced=%lld compositorDartToSwiftP95Ms=%.2f compositorSwiftQueueP95Ms=%.2f compositorReceiveToCompositeP95Ms=%.2f compositorBackend=%@ compositorHz=%.1f compositorTickHz=%.1f compositorTickP95Ms=%.2f compositorFrameP95Ms=%.2f videoAcquireP95Ms=%.2f flutterAcquireP95Ms=%.2f sourceAcquireP95Ms=%.2f drawableAcquireP95Ms=%.2f wgpuSubmitP95Ms=%.2f wgpuCompletionP95Ms=%.2f inFlightSkipHz=%.1f staticSkipHz=%.1f sourceChangeHz=%.1f videoChangeHz=%.1f flutterChangeHz=%.1f wgpuImportUs=%lld wgpuPrepareUs=%lld wgpuBindGroupUs=%lld wgpuPassUs=%lld wgpuSubmitUs=%lld wgpuCpuUs=%lld wgpuDestImport=%lld/%lld wgpuSourceImport=%lld/%lld wgpuCache=%lld evict=%lld wgpuFinalBindGroups=%lld overlayBindGroups=%lld overlayLayer=%lld/%lld",
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
      doubleValue(compositor, "nativeCompositorWgpuSubmitCpuP95Ms"),
      doubleValue(compositor, "nativeCompositorWgpuCompletionP95Ms"),
      doubleValue(compositor, "nativeCompositorInFlightSkipHz"),
      doubleValue(compositor, "nativeCompositorStaticSkipHz"),
      doubleValue(compositor, "nativeCompositorSourceChangeHz"),
      doubleValue(compositor, "nativeCompositorVideoSourceChangeHz"),
      doubleValue(compositor, "nativeCompositorFlutterSourceChangeHz"),
      int64Value(compositor, "nativeCompositorWgpuLastImportUs"),
      int64Value(compositor, "nativeCompositorWgpuLastPrepareUs"),
      int64Value(compositor, "nativeCompositorWgpuLastBindGroupUs"),
      int64Value(compositor, "nativeCompositorWgpuLastPassEncodeUs"),
      int64Value(compositor, "nativeCompositorWgpuLastSubmitUs"),
      int64Value(compositor, "nativeCompositorWgpuLastCpuRenderUs"),
      int64Value(compositor, "nativeCompositorWgpuDestinationImportCount"),
      int64Value(compositor, "nativeCompositorWgpuDestinationImportReuseCount"),
      int64Value(compositor, "nativeCompositorWgpuSourceImportCount"),
      int64Value(compositor, "nativeCompositorWgpuSourceImportReuseCount"),
      int64Value(compositor, "nativeCompositorWgpuImportedTextureCacheSize"),
      int64Value(compositor, "nativeCompositorWgpuImportedTextureCacheEvictionCount"),
      int64Value(compositor, "nativeCompositorWgpuFinalBindGroupCreateCount"),
      int64Value(compositor, "nativeCompositorWgpuOverlayBindGroupCreateCount"),
      int64Value(compositor, "nativeCompositorWgpuOverlayLayerRebuildCount"),
      int64Value(compositor, "nativeCompositorWgpuOverlayLayerReuseCount")
    )
    summary.withCString { pointer in
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
