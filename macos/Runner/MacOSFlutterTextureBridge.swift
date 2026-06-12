import Cocoa
import CoreVideo
import FlutterMacOS

typealias MacOSTextureDiagnostics = (
  rebuildCount: Int,
  reuseCount: Int,
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
  metalBufferExhaustionCount: Int
)

protocol MacOSVideoTexture: FlutterTexture {
  func resize(width: Int, height: Int) -> Bool
  func dimensions() -> (width: Int, height: Int)
  func presentationGeneration() -> Int
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

final class MacOSFlutterTextureBridge: NSObject, MacOSVideoTexture {
  private static let rendererOwnedPixelBufferCount = 4

  private let lock = NSLock()
  private(set) var width: Int
  private(set) var height: Int
  private var pixelFormat: OSType
  private let presentationTarget: MacOSNativeMetalPresentationTarget
  private let hashPrefix: String
  private var pixelBuffers: [CVPixelBuffer] = []
  private var pixelBufferStates: [NativePixelBufferState] = []
  private var resizeFallbackDisplayBuffer: CVPixelBuffer?
  private var displayBufferIndex = 0
  private var drawBufferIndex = 0
  private var lastCopiedBufferIndex: Int?
  private var lastPublishedNativeUploadCount = 0
  private var lastIgnoredNativeUploadCount = 0
  private var metalBufferExhaustionCount = 0
  private var pixelBufferRebuildCount = 0
  private var pixelBufferReuseCount = 0
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
    resizeFallbackDisplayBuffer = pixelBufferLocked(displayBufferIndex)
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
      resizeFallbackDisplayBuffer = nil
      return .alreadyPublished
    }
    guard let publishBufferIndex = pendingBufferIndex,
          pixelBufferStates.indices.contains(publishBufferIndex),
          pixelBufferStates[publishBufferIndex] == .rendering else {
      logTargetStateLocked(reason: "publish-state-mismatch")
      throw MacOSNativePlayerError.transientFrameUnavailable(
        "renderer-owned Metal presentation target changed before publish"
      )
    }
    publishBufferLocked(
      publishBufferIndex,
      nativeUploadCount: pending.publishToken.nativeUploadCount,
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
    lock.lock()
    defer { lock.unlock() }

    let usingResizeFallback = resizeFallbackDisplayBuffer != nil
    guard let pixelBuffer = resizeFallbackDisplayBuffer
      ?? pixelBufferLocked(displayBufferIndex) else { return nil }
    if usingResizeFallback {
      lastCopiedBufferIndex = nil
    } else {
      lastCopiedBufferIndex = displayBufferIndex
      nativeTargetPlayer?.protectMetalPresentationTarget(pixelBuffer)
    }
    return Unmanaged.passRetained(pixelBuffer)
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
    player?.clearMetalPresentationTarget()
    nativeTargetPlayer = nil
    resizeFallbackDisplayBuffer = nil
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

    guard let pixelBuffer = pixelBufferLocked(displayBufferIndex) else {
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
      buffer: pixelBuffer,
      width: width,
      height: height,
      hashPrefix: hashPrefix
    )
  }

  func diagnostics() -> MacOSTextureDiagnostics {
    lock.lock()
    defer { lock.unlock() }

    return (
      rebuildCount: pixelBufferRebuildCount,
      reuseCount: pixelBufferReuseCount,
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
      metalBufferExhaustionCount: metalBufferExhaustionCount
    )
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
      resizeFallbackDisplayBuffer = nil
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
    let attributes = [
      kCVPixelBufferCGImageCompatibilityKey as String: true,
      kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
      kCVPixelBufferMetalCompatibilityKey as String: true,
      kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ] as CFDictionary

    var nextBuffers: [CVPixelBuffer] = []
    for _ in 0..<Self.rendererOwnedPixelBufferCount {
      guard let nextBuffer = makePixelBufferLocked(attributes: attributes) else {
        pixelBuffers = []
        pixelBufferStates = []
        displayBufferIndex = 0
        drawBufferIndex = 0
        lastCopiedBufferIndex = nil
        metalTextureValid = false
        metalTextureLastError = "failed to allocate renderer-owned CVPixelBuffer"
        return
      }
      clearPixelBufferLocked(nextBuffer)
      nextBuffers.append(nextBuffer)
    }
    guard !nextBuffers.isEmpty else {
      pixelBuffers = []
      return
    }
    pixelBufferRebuildCount += 1

    pixelBuffers = nextBuffers
    displayBufferIndex = 0
    drawBufferIndex = nextBuffers.count > 1 ? 1 : 0
    pixelBufferStates = Array(repeating: .available, count: nextBuffers.count)
    pixelBufferStates[displayBufferIndex] = .displayed
    lastCopiedBufferIndex = nil
    lastPublishedNativeUploadCount = 0
    lastIgnoredNativeUploadCount = 0
    nativeTargetPlayer = nil
    validateMetalTextureLocked(buffer: nextBuffers[drawBufferIndex])
  }

  private func makePixelBufferLocked(attributes: CFDictionary) -> CVPixelBuffer? {
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

  private func clearPixelBufferLocked(_ buffer: CVPixelBuffer) {
    if pixelFormat == kCVPixelFormatType_32BGRA {
      MacOSSyntheticTexturePattern.clear(buffer: buffer, width: width, height: height)
      return
    }
    CVPixelBufferLockBaseAddress(buffer, [])
    defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else { return }
    memset(baseAddress, 0, CVPixelBufferGetBytesPerRow(buffer) * CVPixelBufferGetHeight(buffer))
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

  private func publishBufferLocked(
    _ index: Int,
    nativeUploadCount: Int,
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
    pixelBufferMetalUploadCount += 1
    pixelBufferReuseCount += 1
    if let displayBuffer = pixelBufferLocked(displayBufferIndex) {
      player.markMetalPresentationTargetDisplayed(displayBuffer)
      validateMetalTextureLocked(buffer: displayBuffer)
    }
    resizeFallbackDisplayBuffer = nil
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
