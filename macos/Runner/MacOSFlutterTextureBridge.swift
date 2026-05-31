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
  inFlightMetalBufferCount: Int,
  metalBufferExhaustionCount: Int
)

protocol MacOSVideoTexture: FlutterTexture {
  func resize(width: Int, height: Int) -> Bool
  func dimensions() -> (width: Int, height: Int)
  func captureMetrics() -> (
    width: Int,
    height: Int,
    avgLuma: Double,
    nonBlackRatio: Double,
    hash: String,
    regionAvgLuma: [String: Double],
    regionNonBlackRatio: [String: Double]
  )
  func diagnostics() -> MacOSTextureDiagnostics
}

private enum NativePixelBufferState {
  case available
  case inFlight
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
  private let presentationTarget: MacOSNativeMetalPresentationTarget
  private let hashPrefix: String
  private var pixelBuffers: [CVPixelBuffer] = []
  private var pixelBufferStates: [NativePixelBufferState] = []
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

  init(nativeWidth: Int, nativeHeight: Int) {
    self.width = nativeWidth
    self.height = nativeHeight
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
      lock.unlock()
      throw MacOSNativePlayerError.failed("renderer-owned Metal presentation backend is unavailable")
    }
    guard let drawBuffer = markDrawBufferInFlightLocked() else {
      lock.unlock()
      throw MacOSNativePlayerError.transientFrameUnavailable(
        "renderer-owned Metal presentation buffer ring is full"
      )
    }
    guard presentationTarget.install(
      player: player,
      pixelBuffer: drawBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots,
      refresh: false
    ) else {
      pixelBufferMetalUploadFailureCount += 1
      releaseInFlightDrawBufferLocked()
      lock.unlock()
      throw MacOSNativePlayerError.failed("failed to install renderer-owned Metal presentation target")
    }
    lock.unlock()
    let installEndNs = DispatchTime.now().uptimeNanoseconds

    do {
      let requestStartNs = DispatchTime.now().uptimeNanoseconds
      let info = try withExtendedLifetime(drawBuffer) {
        try player.requestRendererOwnedFrameRefresh(timeoutMs: waitTimeoutMs)
      }
      let requestEndNs = DispatchTime.now().uptimeNanoseconds
      lock.lock()
      defer { lock.unlock() }
      let expectedPixelBuffer = Unmanaged.passUnretained(drawBuffer).toOpaque()
      let expectedAddress = UInt(bitPattern: expectedPixelBuffer)
      let completedAddress = info.targetPixelBufferAddress == 0
        ? expectedAddress
        : info.targetPixelBufferAddress
      guard completedAddress == expectedAddress else {
        MacOSProfilerLog.log(String(
          format: "VoidPlayer macOS stale target expected=0x%llx completed=0x%llx display=%d draw=%d states=%@ upload=%d ptsUs=%d",
          UInt64(expectedAddress),
          UInt64(completedAddress),
          displayBufferIndex,
          drawBufferIndex,
          pixelBufferStateSummaryLocked(),
          player.rendererOwnedPresentationUploadCount(),
          info.ptsUs
        ))
        throw MacOSNativePlayerError.transientFrameUnavailable(
          String(
            format: "renderer-owned Metal refresh completed for a stale presentation target expected=0x%llx completed=0x%llx display=%d draw=%d states=%@ upload=%d",
            UInt64(expectedAddress),
            UInt64(completedAddress),
            displayBufferIndex,
            drawBufferIndex,
            pixelBufferStateSummaryLocked(),
            player.rendererOwnedPresentationUploadCount()
          )
        )
      }
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
          nativeUploadCount: player.rendererOwnedPresentationUploadCount()
        )
      )
    } catch {
      let transient = (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true
      let deferredByNativeBackpressure = String(describing: error)
        .contains("renderer-owned Metal async draw deferred by backpressure")
      if transient && deferredByNativeBackpressure {
        lock.lock()
        releaseInFlightDrawBufferLocked()
        lock.unlock()
      }
      if !transient {
        lock.lock()
        pixelBufferMetalUploadFailureCount += 1
        releaseInFlightDrawBufferLocked()
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
      return .alreadyPublished
    }
    guard let publishBufferIndex = pendingBufferIndex,
          pixelBufferStates.indices.contains(publishBufferIndex),
          pixelBufferStates[publishBufferIndex] == .inFlight else {
      throw MacOSNativePlayerError.transientFrameUnavailable(
        "renderer-owned Metal presentation target changed before publish"
      )
    }
    publishBufferLocked(
      publishBufferIndex,
      nativeUploadCount: pending.publishToken.nativeUploadCount
    )
    guard let nextDrawBuffer = pixelBufferLocked(drawBufferIndex),
          presentationTarget.isAvailable(),
          metalTextureValid else {
      throw MacOSNativePlayerError.failed("renderer-owned Metal presentation backend is unavailable")
    }
    markCurrentDrawBufferInFlightLocked()
    if !presentationTarget.install(
      player: player,
      pixelBuffer: nextDrawBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots,
      refresh: false
    ) {
      pixelBufferMetalUploadFailureCount += 1
      releaseInFlightDrawBufferLocked()
      throw MacOSNativePlayerError.failed("failed to install next renderer-owned Metal presentation target")
    }
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
       pixelBufferStates[pendingBufferIndex] == .inFlight {
      pixelBufferStates[pendingBufferIndex] = .available
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

    guard let pixelBuffer = pixelBufferLocked(displayBufferIndex) else { return nil }
    lastCopiedBufferIndex = displayBufferIndex
    return Unmanaged.passRetained(pixelBuffer)
  }

  func dimensions() -> (width: Int, height: Int) {
    lock.lock()
    defer { lock.unlock() }

    return (width: width, height: height)
  }

  func captureMetrics() -> (
    width: Int,
    height: Int,
    avgLuma: Double,
    nonBlackRatio: Double,
    hash: String,
    regionAvgLuma: [String: Double],
    regionNonBlackRatio: [String: Double]
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
        regionNonBlackRatio: [:]
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
      inFlightMetalBufferCount: pixelBufferStates.filter { $0 == .inFlight }.count,
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
    guard let pixelBuffer = markDrawBufferInFlightLocked(allowExistingInFlight: true) else {
      return false
    }
    let installed = presentationTarget.install(
      player: player,
      pixelBuffer: pixelBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots,
      refresh: refresh
    )
    if !installed {
      releaseInFlightDrawBufferLocked()
    }
    return installed
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
      return false
    }
    guard pixelBufferStates.indices.contains(publishBufferIndex),
          pixelBufferStates[publishBufferIndex] == .inFlight else {
      lastIgnoredNativeUploadCount = max(lastIgnoredNativeUploadCount, nativeUploadCount)
      return false
    }
    publishBufferLocked(publishBufferIndex, nativeUploadCount: nativeUploadCount)
    guard let nextDrawBuffer = pixelBufferLocked(drawBufferIndex),
          presentationTarget.isAvailable(),
          metalTextureValid else {
      return false
    }
    markCurrentDrawBufferInFlightLocked()
    if presentationTarget.install(
      player: player,
      pixelBuffer: nextDrawBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots,
      refresh: false
    ) {
      return true
    }
    pixelBufferMetalUploadFailureCount += 1
    releaseInFlightDrawBufferLocked()
    return false
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
      MacOSSyntheticTexturePattern.clear(buffer: nextBuffer, width: width, height: height)
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
    validateMetalTextureLocked(buffer: nextBuffers[drawBufferIndex])
  }

  private func makePixelBufferLocked(attributes: CFDictionary) -> CVPixelBuffer? {
    var nextBuffer: CVPixelBuffer?
    let status = CVPixelBufferCreate(
      kCFAllocatorDefault,
      width,
      height,
      kCVPixelFormatType_32BGRA,
      attributes,
      &nextBuffer
    )
    guard status == kCVReturnSuccess else { return nil }
    return nextBuffer
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
      case .inFlight:
        return "i"
      case .displayed:
        return "d"
      }
    }.joined()
  }

  private func publishBufferLocked(_ index: Int, nativeUploadCount: Int) {
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
      validateMetalTextureLocked(buffer: displayBuffer)
    }
    _ = chooseNextDrawBufferLocked()
  }

  private func markDrawBufferInFlightLocked(
    allowExistingInFlight: Bool = false
  ) -> CVPixelBuffer? {
    if allowExistingInFlight,
       pixelBufferStates.indices.contains(drawBufferIndex),
       pixelBufferStates[drawBufferIndex] == .inFlight {
      return pixelBufferLocked(drawBufferIndex)
    }
    if chooseNextDrawBufferLocked() {
      markCurrentDrawBufferInFlightLocked()
      return pixelBufferLocked(drawBufferIndex)
    }
    return nil
  }

  private func markCurrentDrawBufferInFlightLocked() {
    if pixelBufferStates.indices.contains(drawBufferIndex) {
      pixelBufferStates[drawBufferIndex] = .inFlight
    }
  }

  private func releaseInFlightDrawBufferLocked() {
    if pixelBufferStates.indices.contains(drawBufferIndex),
       pixelBufferStates[drawBufferIndex] == .inFlight {
      pixelBufferStates[drawBufferIndex] = .available
    }
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
    return false
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
      format: "VoidPlayer macOS texture update profiler result=%@ totalMs=%.2f installMs=%.2f requestMs=%.2f publishMs=%.2f timeoutMs=%d ptsUs=%d display=%d draw=%d",
      result,
      Self.ms(totalNs),
      Self.ms(installNs),
      Self.ms(requestNs),
      Self.ms(publishNs),
      waitTimeoutMs,
      ptsUs,
      displayBufferIndex,
      drawBufferIndex
    ))
  }

  private static func ms(_ ns: UInt64) -> Double {
    Double(ns) / 1_000_000.0
  }

}
