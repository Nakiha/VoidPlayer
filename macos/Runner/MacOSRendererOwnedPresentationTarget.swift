import CoreVideo
import FlutterMacOS

typealias MacOSRendererTargetDiagnostics = (
  rebuildCount: Int,
  reuseCount: Int,
  allocationCount: Int,
  rebuildReuseCount: Int,
  rebuildLastAllocatedCount: Int,
  rebuildLastReusedCount: Int,
  rebuildLastDurationMs: Double,
  retiredCount: Int,
  retiredBytes: Int,
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
  rendererOwnedPixelBufferBytes: Int,
  rendererOwnedPixelBufferCount: Int,
  stableDisplayFallbackActive: Bool,
  stableDisplayFallbackCount: Int,
  stableDisplayFallbackPtsUs: Int
)

struct MacOSRendererTargetSnapshot {
  let pixelBuffer: Unmanaged<CVPixelBuffer>
  let generation: Int
  let layoutRevision: UInt64
}

protocol MacOSVideoTexture: FlutterTexture {
  func resize(width: Int, height: Int) -> Bool
  func prewarmRendererTarget(width: Int, height: Int)
  func dimensions() -> (width: Int, height: Int)
  func presentationGeneration() -> Int
  func presentationSnapshot() -> MacOSRendererTargetSnapshot?
  func clearStableDisplaySnapshot()
  func captureMetrics() -> (
    width: Int,
    height: Int,
    avgLuma: Double,
    nonBlackRatio: Double,
    hash: String,
    regionAvgLuma: [String: Double],
    regionNonBlackRatio: [String: Double],
    overlayLinePairedCenters: Int,
    overlayLineWeakWhiteCenters: Int,
    overlayLineBlackOnlyCenters: Int
  )
  func diagnostics() -> MacOSRendererTargetDiagnostics
}

protocol MacOSRendererOwnedPresentationTarget: AnyObject {
  var rendererOwnedRunnerLayerActive: Bool { get }
  func resize(width: Int, height: Int) -> Bool
  func prewarmRendererTarget(width: Int, height: Int)
  func setRendererTargetPixelFormat(
    _ nextPixelFormat: OSType,
    player: MacOSNativePlayerSession?
  ) -> Bool
  func updateFromNativePlayer(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    waitTimeoutMs: Int
  ) throws -> MacOSNativeFrameInfo
  func drawFromNativePlayer(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    waitTimeoutMs: Int
  ) throws -> MacOSPendingNativeFrame
  func publishPendingNativeFrame(
    _ pending: MacOSPendingNativeFrame,
    player: MacOSNativePlayerSession,
    maxTrackSlots: Int
  ) throws -> MacOSNativeFramePublishOutcome
  func discardPendingNativeFrame(_ pending: MacOSPendingNativeFrame)
  func rendererOwnedTargetDiagnostics() -> [String: Any]
  func setRendererOwnedViewportRect(
    left: Int,
    top: Int,
    width: Int,
    height: Int,
    surfaceWidth: Int,
    surfaceHeight: Int
  )
  func installNativePresentationTarget(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    refresh: Bool
  ) -> Bool
  func resetNativeUploadBaseline()
  func publishRenderedTargetAndInstallNext(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    frameInfo: MacOSNativeFrameInfo?
  ) -> MacOSNativeFramePublishOutcome
}

extension MacOSRendererOwnedPresentationTarget {
  var rendererOwnedRunnerLayerActive: Bool {
    false
  }

  func setRendererOwnedViewportRect(
    left: Int,
    top: Int,
    width: Int,
    height: Int,
    surfaceWidth: Int,
    surfaceHeight: Int
  ) {}

  func installNativePresentationTarget(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int
  ) -> Bool {
    installNativePresentationTarget(player, maxTrackSlots: maxTrackSlots, refresh: false)
  }
}

enum MacOSNativeFramePublishOutcome: Equatable {
  case published
  case alreadyPublished
  case notReady
}
