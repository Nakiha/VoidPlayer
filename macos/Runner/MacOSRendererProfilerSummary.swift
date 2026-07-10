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
    let presented = int64Value(presentationFrames, "nativeFramePresentationCount")
    let draws = int64Value(perf, "rendererDrawCount")
    let summary = String(
      format: "playing=%d tracks=%d clock=%@ layout=%.1f/%.1f/%.1fHz layoutP95Ms=%.2f callbacks=%.1fHz presented=%lld draws=%lld drawP95Us=%lld uploadFps=%.1f scheduler=%lld/%lld target=%lld/%lld reuse=%lld prewarm=%lld/%lld compositor=%@@%.1fHz tick=%.1fHz frameP95Ms=%.2f videoAcquireP95Ms=%.2f flutterAcquireP95Ms=%.2f drawableP95Ms=%.2f inFlightSkipHz=%.1f videoChangeHz=%.1f flutterChangeHz=%.1f",
      isPlaying ? 1 : 0,
      trackCount,
      stringValue(viewport, "viewportClockSource", defaultValue: "unknown"),
      doubleValue(viewport, "layoutIntentHz"),
      doubleValue(viewport, "layoutSubmitHz"),
      doubleValue(viewport, "layoutDrawHz"),
      doubleValue(viewport, "layoutRefreshTotalP95Ms"),
      doubleValue(callbacks, "macosFrameCallbackProcessedHz"),
      presented,
      draws,
      int64Value(perf, "rendererDrawP95Us"),
      doubleValue(perf, "rendererOwnedUploadFps"),
      int64Value(scheduler, "tickCount"),
      int64Value(scheduler, "presentableTickCount"),
      int64Value(texture, "pixelBufferRebuildCount"),
      int64Value(texture, "pixelBufferAllocationCount"),
      int64Value(texture, "pixelBufferRebuildReuseCount"),
      int64Value(texture, "pixelBufferPrewarmRequestCount"),
      int64Value(texture, "pixelBufferPrewarmHitCount"),
      stringValue(compositor, "nativeCompositorBackend", defaultValue: "unknown"),
      doubleValue(compositor, "nativeCompositorCompositeHz"),
      doubleValue(compositor, "nativeCompositorDisplayTickHz"),
      doubleValue(compositor, "nativeCompositorFrameCpuP95Ms"),
      doubleValue(compositor, "nativeCompositorVideoAcquireP95Ms"),
      doubleValue(compositor, "nativeCompositorFlutterAcquireP95Ms"),
      doubleValue(compositor, "nativeCompositorDrawableAcquireP95Ms"),
      doubleValue(compositor, "nativeCompositorInFlightSkipHz"),
      doubleValue(compositor, "nativeCompositorVideoSourceChangeHz"),
      doubleValue(compositor, "nativeCompositorFlutterSourceChangeHz")
    )
    summary.withCString { VPMacOSLogProfilerSummary($0) }

    let decodeSummary =
      "DecodeStage decodeAvgMs=\(doubleValue(perf, "decodeAvgMs")) " +
      "decodeMaxMs=\(doubleValue(perf, "decodeMaxMs")) " +
      "recvAvgMs=\(doubleValue(perf, "decodeStageReceiveAvgMs")) " +
      "convertAvgMs=\(doubleValue(perf, "decodeStageConvertAvgMs")) " +
      "softwareFrameStorage=\(stringValue(perf, "softwareFrameStorageKind", defaultValue: "empty")) " +
      "publishAvgMs=\(doubleValue(perf, "decodeStagePublishAvgMs")) " +
      "publishWaitAvgMs=\(doubleValue(perf, "decodeStagePublishWaitAvgMs"))"
    decodeSummary.withCString { VPMacOSLogProfilerSummary($0) }
  }

  private static func doubleValue(
    _ values: [String: Any],
    _ key: String,
    defaultValue: Double = 0.0
  ) -> Double {
    switch values[key] {
    case let value as Double: return value
    case let value as Float: return Double(value)
    case let value as Int: return Double(value)
    case let value as Int64: return Double(value)
    case let value as UInt64: return Double(value)
    default: return defaultValue
    }
  }

  private static func int64Value(
    _ values: [String: Any],
    _ key: String,
    defaultValue: Int64 = 0
  ) -> Int64 {
    switch values[key] {
    case let value as Int64: return value
    case let value as Int: return Int64(value)
    case let value as UInt64: return Int64(min(value, UInt64(Int64.max)))
    case let value as Double: return Int64(value)
    default: return defaultValue
    }
  }

  private static func stringValue(
    _ values: [String: Any],
    _ key: String,
    defaultValue: String
  ) -> String {
    values[key] as? String ?? defaultValue
  }
}
