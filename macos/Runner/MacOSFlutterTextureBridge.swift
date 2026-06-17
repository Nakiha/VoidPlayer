import Cocoa
import CoreVideo
import FlutterMacOS

typealias MacOSTextureDiagnostics = (
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
  rendererOwnedPixelBufferBytes: Int,
  rendererOwnedPixelBufferCount: Int,
  inFlightMetalBufferCount: Int,
  metalBufferExhaustionCount: Int,
  stableDisplayFallbackActive: Bool,
  stableDisplayFallbackCount: Int,
  stableDisplayFallbackPtsUs: Int
)

struct MacOSTexturePresentationSnapshot {
  let pixelBuffer: Unmanaged<CVPixelBuffer>
  let generation: Int
  let layoutRevision: UInt64
}

protocol MacOSVideoTexture: FlutterTexture {
  func resize(width: Int, height: Int) -> Bool
  func prewarmRendererTarget(width: Int, height: Int)
  func dimensions() -> (width: Int, height: Int)
  func presentationGeneration() -> Int
  func presentationSnapshot() -> MacOSTexturePresentationSnapshot?
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
  func diagnostics() -> MacOSTextureDiagnostics
}

private enum NativePixelBufferState: Equatable {
  case available
  // Installed as the native Metal target, but no command is currently writing it.
  case targetInstalled
  // A native/Metal command has accepted this buffer and may still complete.
  case rendering
  case displayed
}

enum MacOSNativeFramePublishOutcome: Equatable {
  case published
  case alreadyPublished
}

private struct MacOSStableDisplaySnapshot {
  let pixelBuffer: CVPixelBuffer
  let width: Int
  let height: Int
  let generation: Int
  let layoutRevision: UInt64
  let ptsUs: Int
}

final class MacOSFlutterTextureBridge: NSObject, MacOSVideoTexture {
  private static let rendererOwnedPixelBufferCount = 4

  private let lock = NSLock()
  private let prewarmQueue = DispatchQueue(
    label: "dev.nakiha.voidplayer.macos.texture-prewarm",
    qos: .userInitiated
  )
  private(set) var width: Int
  private(set) var height: Int
  private var pixelFormat: OSType
  private let presentationTarget: MacOSNativeMetalPresentationTarget
  private let hashPrefix: String
  private var pixelBuffers: [CVPixelBuffer] = []
  private var retiredPixelBuffers: [CVPixelBuffer] = []
  private var pixelBufferStates: [NativePixelBufferState] = []
  private var pixelBufferLayoutRevisions: [UInt64] = []
  private var stableDisplaySnapshot: MacOSStableDisplaySnapshot?
  private var stableDisplayFallbackActive = false
  private var stableDisplayFallbackCount = 0
  private var displayBufferIndex = 0
  private var drawBufferIndex = 0
  private var lastCopiedBufferIndex: Int?
  private var lastPublishedNativeUploadCount = 0
  private var lastIgnoredNativeUploadCount = 0
  private var metalBufferExhaustionCount = 0
  private var pixelBufferRebuildCount = 0
  private var pixelBufferReuseCount = 0
  private var pixelBufferAllocationCount = 0
  private var pixelBufferRebuildReuseCount = 0
  private var pixelBufferRebuildLastAllocatedCount = 0
  private var pixelBufferRebuildLastReusedCount = 0
  private var pixelBufferRebuildLastDurationNs: UInt64 = 0
  private var pixelBufferPrewarmRequestCount = 0
  private var pixelBufferPrewarmHitCount = 0
  private var pixelBufferPrewarmReadyCount = 0
  private var pixelBufferPrewarmDroppedCount = 0
  private var pixelBufferPrewarmKeys: Set<String> = []
  private var pixelBufferMetalUploadCount = 0
  private var pixelBufferMetalUploadFailureCount = 0
  private var metalTextureValid = false
  private var metalTextureCreationCount = 0
  private var metalTextureFailureCount = 0
  private var metalTextureLastError = ""
  private weak var nativeTargetPlayer: MacOSNativePlayerSession?

  init(nativeWidth: Int, nativeHeight: Int) {
    self.width = nativeWidth
    self.height = nativeHeight
    self.pixelFormat = MacOSPresentationConfiguration.current.rendererTargetPixelFormat
    self.presentationTarget = MacOSNativeMetalPresentationTarget(
      width: nativeWidth,
      height: nativeHeight
    )
    self.hashPrefix = "macos-native-frame"
    super.init()
    rebuildPixelBuffer()
  }

  func resize(width: Int, height: Int) -> Bool {
    lock.lock()
    defer { lock.unlock() }

    guard width != self.width || height != self.height else { return false }
    preserveDisplayedBufferAsStableSnapshotLocked()
    self.width = width
    self.height = height
    presentationTarget.resize(width: width, height: height)
    rebuildPixelBuffersLocked()
    return true
  }

  func updateFromNativePlayer(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    waitTimeoutMs: Int
  ) throws -> MacOSNativeFrameInfo {
    let pending = try drawFromNativePlayer(
      player,
      maxTrackSlots: maxTrackSlots,
      waitTimeoutMs: waitTimeoutMs
    )
    _ = try publishPendingNativeFrame(
      pending,
      player: player,
      maxTrackSlots: maxTrackSlots
    )
    return pending.info
  }

  func drawFromNativePlayer(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    waitTimeoutMs: Int
  ) throws -> MacOSPendingNativeFrame {
    let totalStartNs = DispatchTime.now().uptimeNanoseconds
    lock.lock()
    if pixelBuffers.isEmpty {
      rebuildPixelBuffersLocked()
    }
    guard presentationTarget.isAvailable() else {
      logTargetStateLocked(reason: "backend-unavailable")
      lock.unlock()
      throw MacOSNativePlayerError.failed("renderer-owned Metal presentation backend is unavailable")
    }
    guard installTargetRingLocked(player: player, maxTrackSlots: maxTrackSlots, refresh: false)
    else {
      logTargetStateLocked(reason: "install-ring-failed")
      lock.unlock()
      throw MacOSNativePlayerError.failed(
        "failed to install renderer-owned Metal presentation target ring"
      )
    }
    lock.unlock()
    let installEndNs = DispatchTime.now().uptimeNanoseconds

    do {
      let requestStartNs = DispatchTime.now().uptimeNanoseconds
      let info = try player.requestRendererOwnedFrameRefresh(
        timeoutMs: waitTimeoutMs,
        suppressFrameCallback: true
      )
      let requestEndNs = DispatchTime.now().uptimeNanoseconds
      lock.lock()
      defer { lock.unlock() }
      let completedAddress = info.targetPixelBufferAddress
      guard let completedIndex = pixelBufferIndexLocked(address: completedAddress) else {
        throw MacOSNativePlayerError.transientFrameUnavailable(
          "renderer-owned Metal presentation target changed during refresh"
        )
      }
      if pixelBufferStates.indices.contains(completedIndex),
         pixelBufferStates[completedIndex] != .displayed {
        pixelBufferStates[completedIndex] = .rendering
      }
      let totalEndNs = DispatchTime.now().uptimeNanoseconds
      logUpdateProfiler(
        result: "ok",
        totalNs: totalEndNs - totalStartNs,
        installNs: installEndNs - totalStartNs,
        requestNs: requestEndNs - requestStartNs,
        publishNs: 0,
        waitTimeoutMs: waitTimeoutMs,
        ptsUs: info.ptsUs
      )
      return MacOSPendingNativeFrame(
        info: info,
        publishToken: MacOSNativeFramePublishToken(
          pixelBufferAddress: completedAddress,
          nativeUploadCount: player.rendererOwnedPresentationUploadCount()
        )
      )
    } catch {
      let transient = (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true
      if !transient {
        lock.lock()
        pixelBufferMetalUploadFailureCount += 1
        lock.unlock()
        NSLog("VoidPlayer macOS renderer-owned Metal refresh failed: \(error)")
      }
      let nowNs = DispatchTime.now().uptimeNanoseconds
      logUpdateProfiler(
        result: transient ? "coalesced:\(error)" : "error:\(error)",
        totalNs: nowNs - totalStartNs,
        installNs: installEndNs - totalStartNs,
        requestNs: nowNs - installEndNs,
        publishNs: 0,
        waitTimeoutMs: waitTimeoutMs,
        ptsUs: -1
      )
      throw error
    }
  }

  func publishPendingNativeFrame(
    _ pending: MacOSPendingNativeFrame,
    player: MacOSNativePlayerSession,
    maxTrackSlots: Int
  ) throws -> MacOSNativeFramePublishOutcome {
    let publishStartNs = DispatchTime.now().uptimeNanoseconds
    lock.lock()
    defer { lock.unlock() }

    let pendingBufferIndex = pixelBufferIndexLocked(
      address: pending.publishToken.pixelBufferAddress
    )
    let pendingMatchesCurrentDraw = pendingBufferIndex == drawBufferIndex
    let pendingIsDisplayed = pendingBufferIndex == displayBufferIndex

    let callbackUploadFloor = max(
      lastPublishedNativeUploadCount,
      lastIgnoredNativeUploadCount
    )
    guard pending.publishToken.nativeUploadCount > callbackUploadFloor ||
          pendingMatchesCurrentDraw ||
          pendingIsDisplayed else {
      let publishEndNs = DispatchTime.now().uptimeNanoseconds
      logUpdateProfiler(
        result: "already-published",
        totalNs: publishEndNs - publishStartNs,
        installNs: 0,
        requestNs: 0,
        publishNs: publishEndNs - publishStartNs,
        waitTimeoutMs: 0,
        ptsUs: pending.info.ptsUs
      )
      return .alreadyPublished
    }
    if pendingIsDisplayed {
      lastPublishedNativeUploadCount = max(
        lastPublishedNativeUploadCount,
        pending.publishToken.nativeUploadCount
      )
      if let pendingBufferIndex {
        setLayoutRevisionLocked(pending.info.layoutRevision, for: pendingBufferIndex)
        if let buffer = pixelBufferLocked(pendingBufferIndex) {
          recordStableDisplaySnapshotLocked(
            buffer: buffer,
            generation: lastPublishedNativeUploadCount,
            layoutRevision: pending.info.layoutRevision,
            frameInfo: pending.info
          )
        }
      }
      return .alreadyPublished
    }
    guard let publishBufferIndex = pendingBufferIndex,
          pixelBufferStates.indices.contains(publishBufferIndex),
          pixelBufferStates[publishBufferIndex] == .rendering ||
          pixelBufferStates[publishBufferIndex] == .targetInstalled ||
          pixelBufferStates[publishBufferIndex] == .available else {
      logTargetStateLocked(reason: "publish-state-mismatch")
      throw MacOSNativePlayerError.transientFrameUnavailable(
        "renderer-owned Metal presentation target changed before publish"
      )
    }
    publishBufferLocked(
      publishBufferIndex,
      nativeUploadCount: pending.publishToken.nativeUploadCount,
      layoutRevision: pending.info.layoutRevision,
      frameInfo: pending.info,
      player: player
    )
    let publishEndNs = DispatchTime.now().uptimeNanoseconds
    logUpdateProfiler(
      result: "ok",
      totalNs: publishEndNs - publishStartNs,
      installNs: 0,
      requestNs: 0,
      publishNs: publishEndNs - publishStartNs,
      waitTimeoutMs: 0,
      ptsUs: pending.info.ptsUs
    )
    return .published
  }

  func discardPendingNativeFrame(_ pending: MacOSPendingNativeFrame) {
    lock.lock()
    defer { lock.unlock() }

    guard let pendingBufferIndex = pixelBufferIndexLocked(
      address: pending.publishToken.pixelBufferAddress
    ) else {
      return
    }
    if pixelBufferStates.indices.contains(pendingBufferIndex),
       pixelBufferStates[pendingBufferIndex] == .rendering {
      pixelBufferStates[pendingBufferIndex] = .available
      if let buffer = pixelBufferLocked(pendingBufferIndex) {
        nativeTargetPlayer?.releaseMetalPresentationTarget(buffer)
      }
    }
    lastIgnoredNativeUploadCount = max(
      lastIgnoredNativeUploadCount,
      pending.publishToken.nativeUploadCount
    )
    _ = chooseNextDrawBufferLocked()
  }

  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    presentationSnapshot()?.pixelBuffer
  }

  func presentationSnapshot() -> MacOSTexturePresentationSnapshot? {
    lock.lock()
    defer { lock.unlock() }

    if stableDisplayFallbackActive, let snapshot = stableDisplaySnapshot {
      lastCopiedBufferIndex = nil
      return MacOSTexturePresentationSnapshot(
        pixelBuffer: Unmanaged.passRetained(snapshot.pixelBuffer),
        generation: snapshot.generation,
        layoutRevision: snapshot.layoutRevision
      )
    }

    guard let pixelBuffer = pixelBufferLocked(displayBufferIndex) else {
      guard let snapshot = stableDisplaySnapshot else { return nil }
      activateStableDisplayFallbackLocked()
      lastCopiedBufferIndex = nil
      return MacOSTexturePresentationSnapshot(
        pixelBuffer: Unmanaged.passRetained(snapshot.pixelBuffer),
        generation: snapshot.generation,
        layoutRevision: snapshot.layoutRevision
      )
    }
    if lastPublishedNativeUploadCount <= 0, let snapshot = stableDisplaySnapshot {
      activateStableDisplayFallbackLocked()
      lastCopiedBufferIndex = nil
      return MacOSTexturePresentationSnapshot(
        pixelBuffer: Unmanaged.passRetained(snapshot.pixelBuffer),
        generation: snapshot.generation,
        layoutRevision: snapshot.layoutRevision
      )
    }

    let layoutRevision = layoutRevisionLocked(displayBufferIndex)
    if !stableDisplayFallbackActive {
      lastCopiedBufferIndex = displayBufferIndex
      nativeTargetPlayer?.protectMetalPresentationTarget(pixelBuffer)
    }
    return MacOSTexturePresentationSnapshot(
      pixelBuffer: Unmanaged.passRetained(pixelBuffer),
      generation: lastPublishedNativeUploadCount,
      layoutRevision: layoutRevision
    )
  }

  func dimensions() -> (width: Int, height: Int) {
    lock.lock()
    defer { lock.unlock() }

    return (width: width, height: height)
  }

  func setRendererTargetPixelFormat(
    _ nextPixelFormat: OSType,
    player: MacOSNativePlayerSession?
  ) -> Bool {
    lock.lock()
    defer { lock.unlock() }

    guard pixelFormat != nextPixelFormat else { return false }
    nativeTargetPlayer = nil
    preserveDisplayedBufferAsStableSnapshotLocked()
    pixelFormat = nextPixelFormat
    rebuildPixelBuffersLocked()
    return true
  }

  func presentationGeneration() -> Int {
    lock.lock()
    defer { lock.unlock() }

    return lastPublishedNativeUploadCount
  }

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
  ) {
    lock.lock()
    defer { lock.unlock() }

    let captureTarget: (buffer: CVPixelBuffer, width: Int, height: Int)
    if stableDisplayFallbackActive, let snapshot = stableDisplaySnapshot {
      captureTarget = (snapshot.pixelBuffer, snapshot.width, snapshot.height)
    } else if lastPublishedNativeUploadCount <= 0, let snapshot = stableDisplaySnapshot {
      activateStableDisplayFallbackLocked()
      captureTarget = (snapshot.pixelBuffer, snapshot.width, snapshot.height)
    } else if let pixelBuffer = pixelBufferLocked(displayBufferIndex) {
      captureTarget = (pixelBuffer, width, height)
    } else if let snapshot = stableDisplaySnapshot {
      activateStableDisplayFallbackLocked()
      captureTarget = (snapshot.pixelBuffer, snapshot.width, snapshot.height)
    } else {
      return (
        width: width,
        height: height,
        avgLuma: 0.0,
        nonBlackRatio: 0.0,
        hash: "\(hashPrefix)-empty",
        regionAvgLuma: [:],
        regionNonBlackRatio: [:],
        overlayLinePairedCenters: 0,
        overlayLineWeakWhiteCenters: 0,
        overlayLineBlackOnlyCenters: 0
      )
    }
    return MacOSPixelBufferMetrics.capture(
      buffer: captureTarget.buffer,
      width: captureTarget.width,
      height: captureTarget.height,
      hashPrefix: hashPrefix
    )
  }

  func diagnostics() -> MacOSTextureDiagnostics {
    lock.lock()
    defer { lock.unlock() }

    return (
      rebuildCount: pixelBufferRebuildCount,
      reuseCount: pixelBufferReuseCount,
      allocationCount: pixelBufferAllocationCount,
      rebuildReuseCount: pixelBufferRebuildReuseCount,
      rebuildLastAllocatedCount: pixelBufferRebuildLastAllocatedCount,
      rebuildLastReusedCount: pixelBufferRebuildLastReusedCount,
      rebuildLastDurationMs: Double(pixelBufferRebuildLastDurationNs) / 1_000_000.0,
      retiredPixelBufferCount: retiredPixelBuffers.count,
      retiredPixelBufferBytes: retiredPixelBufferBytesLocked(),
      prewarmRequestCount: pixelBufferPrewarmRequestCount,
      prewarmHitCount: pixelBufferPrewarmHitCount,
      prewarmReadyCount: pixelBufferPrewarmReadyCount,
      prewarmDroppedCount: pixelBufferPrewarmDroppedCount,
      metalUploadCount: pixelBufferMetalUploadCount,
      metalUploadFailureCount: pixelBufferMetalUploadFailureCount,
      metalAvailable: presentationTarget.isAvailable(),
      metalTextureCacheAvailable: presentationTarget.isAvailable(),
      metalTextureValid: metalTextureValid,
      metalTextureCreationCount: metalTextureCreationCount,
      metalTextureFailureCount: metalTextureFailureCount,
      metalTextureLastError: metalTextureLastError,
      rendererOwnedPixelBufferBytes: rendererOwnedPixelBufferBytesLocked(),
      rendererOwnedPixelBufferCount: pixelBuffers.count,
      inFlightMetalBufferCount: pixelBufferStates.filter { $0 == .rendering }.count,
      metalBufferExhaustionCount: metalBufferExhaustionCount,
      stableDisplayFallbackActive: stableDisplayFallbackActive,
      stableDisplayFallbackCount: stableDisplayFallbackCount,
      stableDisplayFallbackPtsUs: stableDisplaySnapshot?.ptsUs ?? -1
    )
  }

  func clearStableDisplaySnapshot() {
    lock.lock()
    stableDisplaySnapshot = nil
    stableDisplayFallbackActive = false
    lock.unlock()
  }

  func installNativePresentationTarget(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    refresh: Bool = false
  ) -> Bool {
    lock.lock()
    defer { lock.unlock() }

    guard presentationTarget.isAvailable(),
          metalTextureValid else {
      return false
    }
    return installTargetRingLocked(
      player: player,
      maxTrackSlots: maxTrackSlots,
      refresh: refresh
    )
  }

  func resetNativeUploadBaseline() {
    lock.lock()
    lastPublishedNativeUploadCount = 0
    lastIgnoredNativeUploadCount = 0
    lock.unlock()
  }

  func prewarmRendererTarget(width targetWidth: Int, height targetHeight: Int) {
    let nextWidth = max(16, targetWidth)
    let nextHeight = max(16, targetHeight)
    lock.lock()
    pixelBufferPrewarmRequestCount += 1
    let format = pixelFormat
    let key = "\(nextWidth)x\(nextHeight):\(format)"
    if width == nextWidth && height == nextHeight {
      pixelBufferPrewarmHitCount += 1
      lock.unlock()
      return
    }
    if reusablePixelBufferCountLocked(
      width: nextWidth,
      height: nextHeight,
      pixelFormat: format
    ) >= Self.rendererOwnedPixelBufferCount {
      pixelBufferPrewarmHitCount += 1
      lock.unlock()
      return
    }
    if pixelBufferPrewarmKeys.contains(key) {
      pixelBufferPrewarmHitCount += 1
      lock.unlock()
      return
    }
    pixelBufferPrewarmKeys.insert(key)
    lock.unlock()

    prewarmQueue.async { [weak self] in
      self?.buildPrewarmedRendererTargets(
        width: nextWidth,
        height: nextHeight,
        pixelFormat: format,
        key: key
      )
    }
  }

  func publishRenderedTargetAndInstallNext(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    frameInfo: MacOSNativeFrameInfo?
  ) -> Bool {
    let nativeUploadCount = player.rendererOwnedPresentationUploadCount()
    lock.lock()
    defer { lock.unlock() }

    if nativeUploadCount <= max(lastPublishedNativeUploadCount, lastIgnoredNativeUploadCount) {
      releaseCompletedNativeTargetLocked(
        player: player,
        targetPixelBufferAddress: frameInfo?.targetPixelBufferAddress ?? 0
      )
      return false
    }
    let completedAddress = frameInfo?.targetPixelBufferAddress ?? 0
    let completedBufferIndex = completedAddress == 0
      ? drawBufferIndex
      : pixelBufferIndexLocked(address: completedAddress)
    guard let publishBufferIndex = completedBufferIndex,
          pixelBufferLocked(publishBufferIndex) != nil else {
      lastIgnoredNativeUploadCount = max(lastIgnoredNativeUploadCount, nativeUploadCount)
      return false
    }
    if publishBufferIndex == displayBufferIndex {
      lastPublishedNativeUploadCount = max(lastPublishedNativeUploadCount, nativeUploadCount)
      setLayoutRevisionLocked(frameInfo?.layoutRevision ?? 0, for: publishBufferIndex)
      if let buffer = pixelBufferLocked(publishBufferIndex) {
        recordStableDisplaySnapshotLocked(
          buffer: buffer,
          generation: lastPublishedNativeUploadCount,
          layoutRevision: frameInfo?.layoutRevision ?? 0,
          frameInfo: frameInfo
        )
      }
      return false
    }
    guard pixelBufferStates.indices.contains(publishBufferIndex),
          pixelBufferStates[publishBufferIndex] == .rendering ||
          pixelBufferStates[publishBufferIndex] == .targetInstalled ||
          pixelBufferStates[publishBufferIndex] == .available else {
      releaseCompletedNativeTargetLocked(
        player: player,
        targetPixelBufferAddress: completedAddress
      )
      lastIgnoredNativeUploadCount = max(lastIgnoredNativeUploadCount, nativeUploadCount)
      return false
    }
    publishBufferLocked(
      publishBufferIndex,
      nativeUploadCount: nativeUploadCount,
      layoutRevision: frameInfo?.layoutRevision ?? 0,
      frameInfo: frameInfo,
      player: player
    )
    return true
  }

  private func rebuildPixelBuffer() {
    lock.lock()
    defer { lock.unlock() }

    rebuildPixelBuffersLocked()
  }

  private func rebuildPixelBuffersLocked() {
    let rebuildStartNs = DispatchTime.now().uptimeNanoseconds
    let attributes = [
      kCVPixelBufferCGImageCompatibilityKey as String: true,
      kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
      kCVPixelBufferMetalCompatibilityKey as String: true,
      kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ] as CFDictionary

    var nextBuffers = takeReusablePixelBuffersLocked(count: Self.rendererOwnedPixelBufferCount)
    let reusedBufferCount = nextBuffers.count
    var allocatedBufferCount = 0
    for _ in nextBuffers.count..<Self.rendererOwnedPixelBufferCount {
      guard let nextBuffer = makePixelBuffer(
        width: width,
        height: height,
        pixelFormat: pixelFormat,
        attributes: attributes
      ) else {
        pixelBuffers = []
        pixelBufferStates = []
        pixelBufferLayoutRevisions = []
        displayBufferIndex = 0
        drawBufferIndex = 0
        lastCopiedBufferIndex = nil
        metalTextureValid = false
        metalTextureLastError = "failed to allocate renderer-owned CVPixelBuffer"
        activateStableDisplayFallbackLocked()
        return
      }
      allocatedBufferCount += 1
      clearPixelBuffer(nextBuffer, width: width, height: height, pixelFormat: pixelFormat)
      nextBuffers.append(nextBuffer)
    }
    guard !nextBuffers.isEmpty else {
      pixelBuffers = []
      activateStableDisplayFallbackLocked()
      return
    }
    pixelBufferRebuildCount += 1
    pixelBufferAllocationCount += allocatedBufferCount
    pixelBufferRebuildReuseCount += reusedBufferCount
    pixelBufferRebuildLastAllocatedCount = allocatedBufferCount
    pixelBufferRebuildLastReusedCount = reusedBufferCount
    pixelBufferRebuildLastDurationNs = DispatchTime.now().uptimeNanoseconds - rebuildStartNs

    retireCurrentPixelBuffersLocked()
    pixelBuffers = nextBuffers
    displayBufferIndex = 0
    drawBufferIndex = nextBuffers.count > 1 ? 1 : 0
    pixelBufferStates = Array(repeating: .available, count: nextBuffers.count)
    pixelBufferLayoutRevisions = Array(repeating: 0, count: nextBuffers.count)
    pixelBufferStates[displayBufferIndex] = .displayed
    lastCopiedBufferIndex = nil
    lastPublishedNativeUploadCount = 0
    lastIgnoredNativeUploadCount = 0
    nativeTargetPlayer = nil
    validateMetalTextureLocked(buffer: nextBuffers[drawBufferIndex])
    activateStableDisplayFallbackLocked()
  }

  private func makePixelBuffer(
    width: Int,
    height: Int,
    pixelFormat: OSType,
    attributes: CFDictionary
  ) -> CVPixelBuffer? {
    var nextBuffer: CVPixelBuffer?
    let status = CVPixelBufferCreate(
      kCFAllocatorDefault,
      width,
      height,
      pixelFormat,
      attributes,
      &nextBuffer
    )
    guard status == kCVReturnSuccess else { return nil }
    return nextBuffer
  }

  private func clearPixelBuffer(
    _ buffer: CVPixelBuffer,
    width: Int,
    height: Int,
    pixelFormat: OSType
  ) {
    if pixelFormat == kCVPixelFormatType_32BGRA {
      MacOSSyntheticTexturePattern.clear(buffer: buffer, width: width, height: height)
      return
    }
    CVPixelBufferLockBaseAddress(buffer, [])
    defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else { return }
    memset(baseAddress, 0, CVPixelBufferGetBytesPerRow(buffer) * CVPixelBufferGetHeight(buffer))
  }

  private func buildPrewarmedRendererTargets(
    width targetWidth: Int,
    height targetHeight: Int,
    pixelFormat targetPixelFormat: OSType,
    key: String
  ) {
    let attributes = [
      kCVPixelBufferCGImageCompatibilityKey as String: true,
      kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
      kCVPixelBufferMetalCompatibilityKey as String: true,
      kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ] as CFDictionary
    var buffers: [CVPixelBuffer] = []
    buffers.reserveCapacity(Self.rendererOwnedPixelBufferCount)
    for _ in 0..<Self.rendererOwnedPixelBufferCount {
      guard let buffer = makePixelBuffer(
        width: targetWidth,
        height: targetHeight,
        pixelFormat: targetPixelFormat,
        attributes: attributes
      ) else {
        break
      }
      clearPixelBuffer(
        buffer,
        width: targetWidth,
        height: targetHeight,
        pixelFormat: targetPixelFormat
      )
      buffers.append(buffer)
    }

    lock.lock()
    defer { lock.unlock() }
    pixelBufferPrewarmKeys.remove(key)
    guard buffers.count == Self.rendererOwnedPixelBufferCount else {
      pixelBufferPrewarmDroppedCount += 1
      return
    }
    if reusablePixelBufferCountLocked(
      width: targetWidth,
      height: targetHeight,
      pixelFormat: targetPixelFormat
    ) >= Self.rendererOwnedPixelBufferCount {
      pixelBufferPrewarmHitCount += 1
      return
    }
    appendRetiredPixelBuffersLocked(buffers)
    pixelBufferAllocationCount += buffers.count
    pixelBufferPrewarmReadyCount += buffers.count
  }

  private func validateMetalTextureLocked(buffer: CVPixelBuffer) {
    let validation = presentationTarget.validate(pixelBuffer: buffer, width: width, height: height)
    if validation.valid {
      metalTextureCreationCount += 1
      metalTextureValid = true
      metalTextureLastError = ""
    } else {
      metalTextureFailureCount += 1
      metalTextureValid = false
      metalTextureLastError = validation.error
    }
  }

  private func pixelBufferLocked(_ index: Int) -> CVPixelBuffer? {
    guard pixelBuffers.indices.contains(index) else { return nil }
    return pixelBuffers[index]
  }

  private func pixelBufferIndexLocked(address: UInt) -> Int? {
    for index in pixelBuffers.indices {
      guard let buffer = pixelBufferLocked(index) else { continue }
      if UInt(bitPattern: Unmanaged.passUnretained(buffer).toOpaque()) == address {
        return index
      }
    }
    return nil
  }

  private func layoutRevisionLocked(_ index: Int) -> UInt64 {
    guard pixelBufferLayoutRevisions.indices.contains(index) else { return 0 }
    return pixelBufferLayoutRevisions[index]
  }

  private func setLayoutRevisionLocked(_ revision: UInt64, for index: Int) {
    guard pixelBufferLayoutRevisions.indices.contains(index) else { return }
    pixelBufferLayoutRevisions[index] = revision
  }

  private func preserveDisplayedBufferAsStableSnapshotLocked() {
    guard let buffer = pixelBufferLocked(displayBufferIndex),
          lastPublishedNativeUploadCount > 0 else {
      return
    }
    stableDisplaySnapshot = MacOSStableDisplaySnapshot(
      pixelBuffer: buffer,
      width: CVPixelBufferGetWidth(buffer),
      height: CVPixelBufferGetHeight(buffer),
      generation: lastPublishedNativeUploadCount,
      layoutRevision: layoutRevisionLocked(displayBufferIndex),
      ptsUs: stableDisplaySnapshot?.ptsUs ?? -1
    )
  }

  private func recordStableDisplaySnapshotLocked(
    buffer: CVPixelBuffer,
    generation: Int,
    layoutRevision: UInt64,
    frameInfo: MacOSNativeFrameInfo?
  ) {
    stableDisplaySnapshot = MacOSStableDisplaySnapshot(
      pixelBuffer: buffer,
      width: CVPixelBufferGetWidth(buffer),
      height: CVPixelBufferGetHeight(buffer),
      generation: generation,
      layoutRevision: layoutRevision,
      ptsUs: frameInfo?.ptsUs ?? stableDisplaySnapshot?.ptsUs ?? -1
    )
    stableDisplayFallbackActive = false
  }

  private func activateStableDisplayFallbackLocked() {
    guard stableDisplaySnapshot != nil else {
      stableDisplayFallbackActive = false
      return
    }
    if !stableDisplayFallbackActive {
      stableDisplayFallbackCount += 1
    }
    stableDisplayFallbackActive = true
  }

  private func pixelBufferStateSummaryLocked() -> String {
    pixelBufferStates.map { state in
      switch state {
      case .available:
        return "a"
      case .targetInstalled:
        return "t"
      case .rendering:
        return "r"
      case .displayed:
        return "d"
      }
    }.joined()
  }

  private func rendererOwnedPixelBufferBytesLocked() -> Int {
    pixelBuffers.reduce(0) { total, buffer in
      total + CVPixelBufferGetBytesPerRow(buffer) * CVPixelBufferGetHeight(buffer)
    }
  }

  private func retiredPixelBufferBytesLocked() -> Int {
    retiredPixelBuffers.reduce(0) { total, buffer in
      total + CVPixelBufferGetBytesPerRow(buffer) * CVPixelBufferGetHeight(buffer)
    }
  }

  private func publishBufferLocked(
    _ index: Int,
    nativeUploadCount: Int,
    layoutRevision: UInt64,
    frameInfo: MacOSNativeFrameInfo?,
    player: MacOSNativePlayerSession
  ) {
    if pixelBufferStates.indices.contains(displayBufferIndex) {
      pixelBufferStates[displayBufferIndex] = .available
    }
    displayBufferIndex = index
    if pixelBufferStates.indices.contains(displayBufferIndex) {
      pixelBufferStates[displayBufferIndex] = .displayed
    }
    lastPublishedNativeUploadCount = max(lastPublishedNativeUploadCount, nativeUploadCount)
    setLayoutRevisionLocked(layoutRevision, for: displayBufferIndex)
    pixelBufferMetalUploadCount += 1
    pixelBufferReuseCount += 1
    if let displayBuffer = pixelBufferLocked(displayBufferIndex) {
      player.markMetalPresentationTargetDisplayed(displayBuffer)
      validateMetalTextureLocked(buffer: displayBuffer)
      recordStableDisplaySnapshotLocked(
        buffer: displayBuffer,
        generation: lastPublishedNativeUploadCount,
        layoutRevision: layoutRevision,
        frameInfo: frameInfo
      )
    }
    _ = chooseNextDrawBufferLocked()
  }

  @discardableResult
  private func installTargetRingLocked(
    player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    refresh: Bool
  ) -> Bool {
    guard presentationTarget.isAvailable(),
          metalTextureValid,
          !pixelBuffers.isEmpty else {
      return false
    }
    nativeTargetPlayer = player
    let displayedBuffer = pixelBufferLocked(displayBufferIndex)
    let protectedBuffer = lastCopiedBufferIndex.flatMap { pixelBufferLocked($0) }
    let installed = presentationTarget.installRing(
      player: player,
      pixelBuffers: pixelBuffers,
      displayedPixelBuffer: displayedBuffer,
      protectedPixelBuffer: protectedBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots
    )
    if !installed {
      pixelBufferMetalUploadFailureCount += 1
      logTargetStateLocked(reason: "native-install-ring-returned-false")
    }
    if refresh, installed {
      player.protectMetalPresentationTarget(protectedBuffer)
    }
    return installed
  }

  private func releaseCompletedNativeTargetLocked(
    player: MacOSNativePlayerSession,
    targetPixelBufferAddress: UInt
  ) {
    guard targetPixelBufferAddress != 0,
          let index = pixelBufferIndexLocked(address: targetPixelBufferAddress),
          index != displayBufferIndex,
          let buffer = pixelBufferLocked(index) else {
      return
    }
    if pixelBufferStates.indices.contains(index),
       pixelBufferStates[index] == .rendering {
      pixelBufferStates[index] = .available
    }
    player.releaseMetalPresentationTarget(buffer)
  }

  @discardableResult
  private func chooseNextDrawBufferLocked() -> Bool {
    guard !pixelBuffers.isEmpty else {
      drawBufferIndex = 0
      return false
    }
    for index in pixelBuffers.indices
      where index != displayBufferIndex &&
        index != lastCopiedBufferIndex &&
        pixelBufferStates.indices.contains(index) &&
        pixelBufferStates[index] == .available {
      drawBufferIndex = index
      return true
    }
    metalBufferExhaustionCount += 1
    logTargetStateLocked(reason: "buffer-exhausted")
    return false
  }

  private func pixelBufferAddressLocked(_ index: Int) -> UInt {
    guard let buffer = pixelBufferLocked(index) else { return 0 }
    return UInt(bitPattern: Unmanaged.passUnretained(buffer).toOpaque())
  }

  private func retireCurrentPixelBuffersLocked() {
    guard !pixelBuffers.isEmpty else { return }
    appendRetiredPixelBuffersLocked(pixelBuffers)
  }

  private func appendRetiredPixelBuffersLocked(_ buffers: [CVPixelBuffer]) {
    guard !buffers.isEmpty else { return }
    retiredPixelBuffers.append(contentsOf: buffers)
    let maxRetiredBuffers = Self.rendererOwnedPixelBufferCount * 2
    if retiredPixelBuffers.count > maxRetiredBuffers {
      retiredPixelBuffers.removeFirst(retiredPixelBuffers.count - maxRetiredBuffers)
    }
  }

  private func reusablePixelBufferCountLocked(
    width targetWidth: Int,
    height targetHeight: Int,
    pixelFormat targetPixelFormat: OSType
  ) -> Int {
    retiredPixelBuffers.filter {
      CVPixelBufferGetWidth($0) == targetWidth &&
        CVPixelBufferGetHeight($0) == targetHeight &&
        CVPixelBufferGetPixelFormatType($0) == targetPixelFormat
    }.count
  }

  private func takeReusablePixelBuffersLocked(count: Int) -> [CVPixelBuffer] {
    guard count > 0, !retiredPixelBuffers.isEmpty else { return [] }
    var reusable: [CVPixelBuffer] = []
    var remaining: [CVPixelBuffer] = []
    reusable.reserveCapacity(count)
    remaining.reserveCapacity(retiredPixelBuffers.count)
    for buffer in retiredPixelBuffers {
      if reusable.count < count,
         CVPixelBufferGetWidth(buffer) == width,
         CVPixelBufferGetHeight(buffer) == height,
         CVPixelBufferGetPixelFormatType(buffer) == pixelFormat {
        reusable.append(buffer)
      } else {
        remaining.append(buffer)
      }
    }
    retiredPixelBuffers = remaining
    return reusable
  }

  private func logTargetStateLocked(reason: String) {
    NSLog(
      "VoidPlayer macOS renderer target state reason=%@ size=%dx%d format=%u display=%d draw=%d copied=%d states=%@ buffers=%@ published=%d ignored=%d failures=%d exhausted=%d metalValid=%@ metalError=%@",
      reason,
      width,
      height,
      pixelFormat,
      displayBufferIndex,
      drawBufferIndex,
      lastCopiedBufferIndex ?? -1,
      pixelBufferStateSummaryLocked(),
      pixelBuffers.indices.map { String(format: "0x%llx", UInt64(pixelBufferAddressLocked($0))) }
        .joined(separator: ","),
      lastPublishedNativeUploadCount,
      lastIgnoredNativeUploadCount,
      pixelBufferMetalUploadFailureCount,
      metalBufferExhaustionCount,
      metalTextureValid ? "true" : "false",
      metalTextureLastError
    )
  }

  private func logUpdateProfiler(
    result: String,
    totalNs: UInt64,
    installNs: UInt64,
    requestNs: UInt64,
    publishNs: UInt64,
    waitTimeoutMs: Int,
    ptsUs: Int
  ) {
    let isError = result.hasPrefix("error:")
    let slow = totalNs >= 12_000_000 || requestNs >= 10_000_000 || isError
    guard slow else { return }
    MacOSProfilerLog.log(String(
      format: "VoidPlayer macOS texture update profiler result=%@ totalMs=%.2f installMs=%.2f requestMs=%.2f publishMs=%.2f timeoutMs=%d ptsUs=%d display=%d draw=%d states=%@",
      result,
      Self.ms(totalNs),
      Self.ms(installNs),
      Self.ms(requestNs),
      Self.ms(publishNs),
      waitTimeoutMs,
      ptsUs,
      displayBufferIndex,
      drawBufferIndex,
      pixelBufferStateSummaryLocked()
    ))
  }

  private static func ms(_ ns: UInt64) -> Double {
    Double(ns) / 1_000_000.0
  }

}
