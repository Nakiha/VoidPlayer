import Cocoa
import CoreVideo
import FlutterMacOS

private struct MacOSRendererTargetDisplaySnapshot {
  let pixelBuffer: CVPixelBuffer
  let width: Int
  let height: Int
  let generation: Int
  let layoutRevision: UInt64
  let ptsUs: Int
}

final class MacOSRendererOwnedTextureTarget: NSObject, MacOSVideoTexture,
  MacOSRendererOwnedPresentationTarget {
  private static let rendererOwnedPixelBufferCount = 6

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
  private var pixelBufferLayoutRevisions: [UInt64] = []
  private var pixelBufferGeneration = 0
  private var stableDisplaySnapshot: MacOSRendererTargetDisplaySnapshot?
  private var stableDisplayFallbackActive = false
  private var stableDisplayFallbackCount = 0
  private var displayBufferIndex = 0
  private var lastCopiedBufferIndex: Int?
  private var lastPublishedNativeUploadCount = 0
  private var lastIgnoredNativeUploadCount = 0
  private var rendererTargetRebuildCount = 0
  private var rendererTargetReuseCount = 0
  private var rendererTargetAllocationCount = 0
  private var rendererTargetRebuildReuseCount = 0
  private var rendererTargetRebuildLastAllocatedCount = 0
  private var rendererTargetRebuildLastReusedCount = 0
  private var rendererTargetRebuildLastDurationNs: UInt64 = 0
  private var rendererTargetPrewarmRequestCount = 0
  private var rendererTargetPrewarmHitCount = 0
  private var rendererTargetPrewarmReadyCount = 0
  private var rendererTargetPrewarmDroppedCount = 0
  private var rendererTargetPrewarmKeys: Set<String> = []
  private var rendererTargetUploadCount = 0
  private var rendererTargetUploadFailureCount = 0
  private var metalTextureValid = false
  private var metalTextureCreationCount = 0
  private var metalTextureFailureCount = 0
  private var metalTextureLastError = ""
  private weak var nativeTargetPlayer: MacOSNativePlayerSession?

  init(
    nativeWidth: Int,
    nativeHeight: Int,
    pixelFormatOverride: OSType? = nil
  ) {
    self.width = nativeWidth
    self.height = nativeHeight
    self.pixelFormat =
      pixelFormatOverride ?? MacOSPresentationConfiguration.current.rendererTargetPixelFormat
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
      guard pixelBufferIndexLocked(address: completedAddress) != nil else {
        throw MacOSNativePlayerError.transientFrameUnavailable(
          "renderer-owned Metal presentation target changed during refresh"
        )
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
          nativeUploadCount: player.rendererOwnedPresentationUploadCount(),
          pixelBufferGeneration: pixelBufferGeneration
        )
      )
    } catch {
      let transient = (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true
      if !transient {
        lock.lock()
        rendererTargetUploadFailureCount += 1
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

    guard pending.publishToken.pixelBufferGeneration == pixelBufferGeneration else {
      lastIgnoredNativeUploadCount = max(
        lastIgnoredNativeUploadCount,
        pending.publishToken.nativeUploadCount
      )
      throw MacOSNativePlayerError.transientFrameUnavailable(
        "renderer-owned Metal presentation target generation changed before publish"
      )
    }
    let pendingBufferIndex = pixelBufferIndexLocked(
      address: pending.publishToken.pixelBufferAddress
    )
    let pendingIsDisplayed = pendingBufferIndex == displayBufferIndex

    let callbackUploadFloor = max(
      lastPublishedNativeUploadCount,
      lastIgnoredNativeUploadCount
    )
    guard pending.publishToken.nativeUploadCount > callbackUploadFloor ||
          pendingIsDisplayed else {
      releaseCompletedNativeTargetLocked(
        player: player,
        targetPixelBufferAddress: pending.publishToken.pixelBufferAddress
      )
      releaseReusableNativeTargetsLocked(player: player)
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
      releaseReusableNativeTargetsLocked(player: player)
      return .alreadyPublished
    }
    guard let publishBufferIndex = pendingBufferIndex else {
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

    guard pending.publishToken.pixelBufferGeneration == pixelBufferGeneration else {
      lastIgnoredNativeUploadCount = max(
        lastIgnoredNativeUploadCount,
        pending.publishToken.nativeUploadCount
      )
      return
    }
    guard let pendingBufferIndex = pixelBufferIndexLocked(
      address: pending.publishToken.pixelBufferAddress
    ) else {
      return
    }
    if pendingBufferIndex != displayBufferIndex,
       let buffer = pixelBufferLocked(pendingBufferIndex) {
      nativeTargetPlayer?.releaseMetalPresentationTarget(buffer)
    }
    lastIgnoredNativeUploadCount = max(
      lastIgnoredNativeUploadCount,
      pending.publishToken.nativeUploadCount
    )
    if let player = nativeTargetPlayer {
      releaseReusableNativeTargetsLocked(player: player)
    }
  }

  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    presentationSnapshot()?.pixelBuffer
  }

  func presentationSnapshot() -> MacOSRendererTargetSnapshot? {
    lock.lock()
    defer { lock.unlock() }

    if stableDisplayFallbackActive, let snapshot = stableDisplaySnapshot {
      lastCopiedBufferIndex = nil
      return MacOSRendererTargetSnapshot(
        pixelBuffer: Unmanaged.passRetained(snapshot.pixelBuffer),
        generation: snapshot.generation,
        layoutRevision: snapshot.layoutRevision
      )
    }

    guard let pixelBuffer = pixelBufferLocked(displayBufferIndex) else {
      guard let snapshot = stableDisplaySnapshot else { return nil }
      activateStableDisplayFallbackLocked()
      lastCopiedBufferIndex = nil
      return MacOSRendererTargetSnapshot(
        pixelBuffer: Unmanaged.passRetained(snapshot.pixelBuffer),
        generation: snapshot.generation,
        layoutRevision: snapshot.layoutRevision
      )
    }
    if lastPublishedNativeUploadCount <= 0, let snapshot = stableDisplaySnapshot {
      activateStableDisplayFallbackLocked()
      lastCopiedBufferIndex = nil
      return MacOSRendererTargetSnapshot(
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
    return MacOSRendererTargetSnapshot(
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

  func diagnostics() -> MacOSRendererTargetDiagnostics {
    lock.lock()
    defer { lock.unlock() }

    return (
      rebuildCount: rendererTargetRebuildCount,
      reuseCount: rendererTargetReuseCount,
      allocationCount: rendererTargetAllocationCount,
      rebuildReuseCount: rendererTargetRebuildReuseCount,
      rebuildLastAllocatedCount: rendererTargetRebuildLastAllocatedCount,
      rebuildLastReusedCount: rendererTargetRebuildLastReusedCount,
      rebuildLastDurationMs: Double(rendererTargetRebuildLastDurationNs) / 1_000_000.0,
      retiredCount: retiredPixelBuffers.count,
      retiredBytes: rendererTargetRetiredBytesLocked(),
      prewarmRequestCount: rendererTargetPrewarmRequestCount,
      prewarmHitCount: rendererTargetPrewarmHitCount,
      prewarmReadyCount: rendererTargetPrewarmReadyCount,
      prewarmDroppedCount: rendererTargetPrewarmDroppedCount,
      metalUploadCount: rendererTargetUploadCount,
      metalUploadFailureCount: rendererTargetUploadFailureCount,
      metalAvailable: presentationTarget.isAvailable(),
      metalTextureCacheAvailable: presentationTarget.isAvailable(),
      metalTextureValid: metalTextureValid,
      metalTextureCreationCount: metalTextureCreationCount,
      metalTextureFailureCount: metalTextureFailureCount,
      metalTextureLastError: metalTextureLastError,
      rendererOwnedPixelBufferBytes: rendererOwnedPixelBufferBytesLocked(),
      rendererOwnedPixelBufferCount: pixelBuffers.count,
      stableDisplayFallbackActive: stableDisplayFallbackActive,
      stableDisplayFallbackCount: stableDisplayFallbackCount,
      stableDisplayFallbackPtsUs: stableDisplaySnapshot?.ptsUs ?? -1
    )
  }

  func rendererOwnedTargetDiagnostics() -> [String: Any] {
    lock.lock()
    defer { lock.unlock() }

    let target = pixelBufferLocked(displayBufferIndex) ?? stableDisplaySnapshot?.pixelBuffer
    let format = target.map { CVPixelBufferGetPixelFormatType($0) } ?? pixelFormat
    let edrMetrics = Self.measureEDRTarget(pixelBuffer: target, format: format)
    return [
      "rendererOwnedTargetPixelFormat": Self.pixelFormatName(format),
      "rendererOwnedEDROutputEnabled": format == kCVPixelFormatType_64RGBAHalf,
      "rendererOwnedEDRTargetSampleCount": edrMetrics.sampleCount,
      "rendererOwnedEDRTargetMaxRGBX1000": edrMetrics.maxRGBX1000,
      "rendererOwnedEDRTargetPixelsOver1X1000": edrMetrics.pixelsOver1X1000,
    ]
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
    rendererTargetPrewarmRequestCount += 1
    let format = pixelFormat
    let key = "\(nextWidth)x\(nextHeight):\(format)"
    if width == nextWidth && height == nextHeight {
      rendererTargetPrewarmHitCount += 1
      lock.unlock()
      return
    }
    if reusablePixelBufferCountLocked(
      width: nextWidth,
      height: nextHeight,
      pixelFormat: format
    ) >= Self.rendererOwnedPixelBufferCount {
      rendererTargetPrewarmHitCount += 1
      lock.unlock()
      return
    }
    if rendererTargetPrewarmKeys.contains(key) {
      rendererTargetPrewarmHitCount += 1
      lock.unlock()
      return
    }
    rendererTargetPrewarmKeys.insert(key)
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
  ) -> MacOSNativeFramePublishOutcome {
    let nativeUploadCount = player.rendererOwnedPresentationUploadCount()
    lock.lock()
    defer { lock.unlock() }

    if nativeUploadCount <= max(lastPublishedNativeUploadCount, lastIgnoredNativeUploadCount) {
      releaseCompletedNativeTargetLocked(
        player: player,
        targetPixelBufferAddress: frameInfo?.targetPixelBufferAddress ?? 0
      )
      releaseReusableNativeTargetsLocked(player: player)
      return .notReady
    }
    let completedAddress = frameInfo?.targetPixelBufferAddress ?? 0
    let completedBufferIndex = pixelBufferIndexLocked(address: completedAddress)
    guard let publishBufferIndex = completedBufferIndex,
          pixelBufferLocked(publishBufferIndex) != nil else {
      lastIgnoredNativeUploadCount = max(lastIgnoredNativeUploadCount, nativeUploadCount)
      return .notReady
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
      releaseReusableNativeTargetsLocked(player: player)
      return .alreadyPublished
    }
    publishBufferLocked(
      publishBufferIndex,
      nativeUploadCount: nativeUploadCount,
      layoutRevision: frameInfo?.layoutRevision ?? 0,
      frameInfo: frameInfo,
      player: player
    )
    return .published
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
        pixelBufferLayoutRevisions = []
        displayBufferIndex = 0
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
    rendererTargetRebuildCount += 1
    pixelBufferGeneration &+= 1
    rendererTargetAllocationCount += allocatedBufferCount
    rendererTargetRebuildReuseCount += reusedBufferCount
    rendererTargetRebuildLastAllocatedCount = allocatedBufferCount
    rendererTargetRebuildLastReusedCount = reusedBufferCount
    rendererTargetRebuildLastDurationNs = DispatchTime.now().uptimeNanoseconds - rebuildStartNs

    retireCurrentPixelBuffersLocked()
    pixelBuffers = nextBuffers
    displayBufferIndex = 0
    pixelBufferLayoutRevisions = Array(repeating: 0, count: nextBuffers.count)
    lastCopiedBufferIndex = nil
    lastPublishedNativeUploadCount = 0
    lastIgnoredNativeUploadCount = 0
    nativeTargetPlayer = nil
    validateMetalTextureLocked(buffer: nextBuffers[displayBufferIndex])
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
    rendererTargetPrewarmKeys.remove(key)
    guard buffers.count == Self.rendererOwnedPixelBufferCount else {
      rendererTargetPrewarmDroppedCount += 1
      return
    }
    if reusablePixelBufferCountLocked(
      width: targetWidth,
      height: targetHeight,
      pixelFormat: targetPixelFormat
    ) >= Self.rendererOwnedPixelBufferCount {
      rendererTargetPrewarmHitCount += 1
      return
    }
    appendRetiredPixelBuffersLocked(buffers)
    rendererTargetAllocationCount += buffers.count
    rendererTargetPrewarmReadyCount += buffers.count
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
    stableDisplaySnapshot = MacOSRendererTargetDisplaySnapshot(
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
    stableDisplaySnapshot = MacOSRendererTargetDisplaySnapshot(
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

  private func rendererOwnedPixelBufferBytesLocked() -> Int {
    pixelBuffers.reduce(0) { total, buffer in
      total + CVPixelBufferGetBytesPerRow(buffer) * CVPixelBufferGetHeight(buffer)
    }
  }

  private func rendererTargetRetiredBytesLocked() -> Int {
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
    displayBufferIndex = index
    lastPublishedNativeUploadCount = max(lastPublishedNativeUploadCount, nativeUploadCount)
    setLayoutRevisionLocked(layoutRevision, for: displayBufferIndex)
    rendererTargetUploadCount += 1
    rendererTargetReuseCount += 1
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
    releaseReusableNativeTargetsLocked(player: player)
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
      rendererTargetUploadFailureCount += 1
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
    player.releaseMetalPresentationTarget(buffer)
  }

  private func releaseReusableNativeTargetsLocked(player: MacOSNativePlayerSession) {
    for index in pixelBuffers.indices {
      guard index != displayBufferIndex,
            index != lastCopiedBufferIndex,
            let buffer = pixelBufferLocked(index) else {
        continue
      }
      player.releaseMetalPresentationTarget(buffer)
    }
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
      "VoidPlayer macOS renderer target state reason=%@ size=%dx%d format=%u display=%d copied=%d buffers=%@ published=%d ignored=%d failures=%d metalValid=%@ metalError=%@",
      reason,
      width,
      height,
      pixelFormat,
      displayBufferIndex,
      lastCopiedBufferIndex ?? -1,
      pixelBuffers.indices.map { String(format: "0x%llx", UInt64(pixelBufferAddressLocked($0))) }
        .joined(separator: ","),
      lastPublishedNativeUploadCount,
      lastIgnoredNativeUploadCount,
      rendererTargetUploadFailureCount,
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
      format: "VoidPlayer macOS texture update profiler result=%@ totalMs=%.2f installMs=%.2f requestMs=%.2f publishMs=%.2f timeoutMs=%d ptsUs=%d display=%d",
      result,
      Self.ms(totalNs),
      Self.ms(installNs),
      Self.ms(requestNs),
      Self.ms(publishNs),
      waitTimeoutMs,
      ptsUs,
      displayBufferIndex
    ))
  }

  private static func ms(_ ns: UInt64) -> Double {
    Double(ns) / 1_000_000.0
  }

  private static func pixelFormatName(_ format: OSType) -> String {
    switch format {
    case kCVPixelFormatType_32BGRA:
      return "32BGRA"
    case kCVPixelFormatType_64RGBAHalf:
      return "64RGBAHalf"
    default:
      return String(format)
    }
  }

  private static func measureEDRTarget(
    pixelBuffer: CVPixelBuffer?,
    format: OSType
  ) -> (sampleCount: Int, maxRGBX1000: Int, pixelsOver1X1000: Int) {
    guard format == kCVPixelFormatType_64RGBAHalf,
          let pixelBuffer else {
      return (sampleCount: 0, maxRGBX1000: 0, pixelsOver1X1000: 0)
    }
    guard CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly) == kCVReturnSuccess else {
      return (sampleCount: 0, maxRGBX1000: 0, pixelsOver1X1000: 0)
    }
    defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
    guard let baseAddress = CVPixelBufferGetBaseAddress(pixelBuffer) else {
      return (sampleCount: 0, maxRGBX1000: 0, pixelsOver1X1000: 0)
    }
    let width = CVPixelBufferGetWidth(pixelBuffer)
    let height = CVPixelBufferGetHeight(pixelBuffer)
    guard width > 0, height > 0 else {
      return (sampleCount: 0, maxRGBX1000: 0, pixelsOver1X1000: 0)
    }
    let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
    let words = baseAddress.assumingMemoryBound(to: UInt16.self)
    let sampleColumns = min(96, width)
    let sampleRows = min(96, height)
    var sampleCount = 0
    var overOneCount = 0
    var maxRGB: Float = 0.0
    for row in 0..<sampleRows {
      let y = sampleRows == 1 ? 0 : row * (height - 1) / (sampleRows - 1)
      let rowOffset = y * bytesPerRow / MemoryLayout<UInt16>.stride
      for column in 0..<sampleColumns {
        let x = sampleColumns == 1 ? 0 : column * (width - 1) / (sampleColumns - 1)
        let pixelOffset = rowOffset + x * 4
        let r = Self.floatFromHalf(words[pixelOffset])
        let g = Self.floatFromHalf(words[pixelOffset + 1])
        let b = Self.floatFromHalf(words[pixelOffset + 2])
        let pixelMax = max(r, max(g, b))
        maxRGB = max(maxRGB, pixelMax)
        if pixelMax > 1.0 {
          overOneCount += 1
        }
        sampleCount += 1
      }
    }
    return (
      sampleCount: sampleCount,
      maxRGBX1000: Int((maxRGB * 1000.0).rounded()),
      pixelsOver1X1000: sampleCount > 0 ? overOneCount * 1000 / sampleCount : 0
    )
  }

  private static func floatFromHalf(_ value: UInt16) -> Float {
    let sign = (UInt32(value & 0x8000)) << 16
    let exponent = Int((value & 0x7C00) >> 10)
    let mantissa = UInt32(value & 0x03FF)
    let bits: UInt32
    if exponent == 0 {
      if mantissa == 0 {
        bits = sign
      } else {
        var normalizedMantissa = mantissa
        var normalizedExponent = -14
        while (normalizedMantissa & 0x0400) == 0 {
          normalizedMantissa <<= 1
          normalizedExponent -= 1
        }
        normalizedMantissa &= 0x03FF
        bits = sign |
          (UInt32(normalizedExponent + 127) << 23) |
          (normalizedMantissa << 13)
      }
    } else if exponent == 0x1F {
      bits = sign | 0x7F800000 | (mantissa << 13)
    } else {
      bits = sign |
        (UInt32(exponent - 15 + 127) << 23) |
        (mantissa << 13)
    }
    return Float(bitPattern: bits)
  }

}
