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
  metalTextureLastError: String
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

final class MacOSNativeMetalPresentationTarget {
  private var backend: OpaquePointer?

  init(width: Int, height: Int) {
    recreate(width: width, height: height)
  }

  deinit {
    if let backend {
      VPMacOSMetalPresentationBackendDestroy(backend)
    }
  }

  func resize(width: Int, height: Int) {
    recreate(width: width, height: height)
  }

  func isAvailable() -> Bool {
    guard let backend else { return false }
    return VPMacOSMetalPresentationBackendIsAvailable(backend) != 0
  }

  func install(
    player: MacOSNativePlayerSession,
    pixelBuffer: CVPixelBuffer,
    width: Int,
    height: Int,
    maxTrackSlots: Int,
    refresh: Bool = true
  ) -> Bool {
    guard let backend, isAvailable() else { return false }
    return player.setMetalPresentationTarget(
      backend: backend,
      pixelBuffer: pixelBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots,
      refresh: refresh
    )
  }

  func validate(pixelBuffer: CVPixelBuffer, width: Int, height: Int) -> (valid: Bool, error: String) {
    guard let backend else {
      return (valid: false, error: "native Metal presentation backend is null")
    }
    var error = [CChar](repeating: 0, count: 512)
    let status = VPMacOSMetalPresentationBackendValidatePixelBufferChecked(
      backend,
      UnsafeMutableRawPointer(Unmanaged.passUnretained(pixelBuffer).toOpaque()),
      Int32(width),
      Int32(height),
      &error,
      error.count
    )
    return status == 0
      ? (valid: true, error: "")
      : (valid: false, error: String(cString: error))
  }

  private func recreate(width: Int, height: Int) {
    if let backend {
      VPMacOSMetalPresentationBackendDestroy(backend)
    }
    backend = VPMacOSMetalPresentationBackendCreate(Int32(width), Int32(height))
  }
}

final class MacOSFlutterTextureBridge: NSObject, MacOSVideoTexture {
  private let lock = NSLock()
  private(set) var width: Int
  private(set) var height: Int
  private let presentationTarget: MacOSNativeMetalPresentationTarget
  private let hashPrefix: String
  private var pixelBuffers: [CVPixelBuffer] = []
  private var displayBufferIndex = 0
  private var drawBufferIndex = 0
  private var lastCopiedBufferIndex: Int?
  private var lastPublishedNativeUploadCount = 0
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
    let totalStartNs = DispatchTime.now().uptimeNanoseconds
    lock.lock()
    if pixelBuffers.isEmpty {
      rebuildPixelBuffersLocked()
    }
    guard let drawBuffer = pixelBufferLocked(drawBufferIndex) else {
      lock.unlock()
      throw MacOSNativePlayerError.invalidPayload
    }
    guard presentationTarget.isAvailable() else {
      lock.unlock()
      throw MacOSNativePlayerError.failed("renderer-owned Metal presentation backend is unavailable")
    }
    guard presentationTarget.install(
      player: player,
      pixelBuffer: drawBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots
    ) else {
      pixelBufferMetalUploadFailureCount += 1
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
      guard let currentDrawBuffer = pixelBufferLocked(drawBufferIndex),
            Unmanaged.passUnretained(currentDrawBuffer).toOpaque() == expectedPixelBuffer else {
        throw MacOSNativePlayerError.failed("renderer-owned Metal presentation target changed during refresh")
      }
      publishDrawBufferLocked(nativeUploadCount: player.rendererOwnedPresentationUploadCount())
      guard let nextDrawBuffer = pixelBufferLocked(drawBufferIndex),
            presentationTarget.install(
              player: player,
              pixelBuffer: nextDrawBuffer,
              width: width,
              height: height,
              maxTrackSlots: maxTrackSlots,
              refresh: false
            ) else {
        pixelBufferMetalUploadFailureCount += 1
        throw MacOSNativePlayerError.failed("failed to install next renderer-owned Metal presentation target")
      }
      let totalEndNs = DispatchTime.now().uptimeNanoseconds
      logUpdateProfiler(
        result: "ok",
        totalNs: totalEndNs - totalStartNs,
        installNs: installEndNs - totalStartNs,
        requestNs: requestEndNs - requestStartNs,
        publishNs: totalEndNs - requestEndNs,
        waitTimeoutMs: waitTimeoutMs,
        ptsUs: info.ptsUs
      )
      return info
    } catch {
      if (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable != true {
        lock.lock()
        pixelBufferMetalUploadFailureCount += 1
        lock.unlock()
        NSLog("VoidPlayer macOS renderer-owned Metal refresh failed: \(error)")
      }
      let nowNs = DispatchTime.now().uptimeNanoseconds
      logUpdateProfiler(
        result: "error:\(error)",
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
      metalTextureLastError: metalTextureLastError
    )
  }

  func installNativePresentationTarget(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    refresh: Bool = true
  ) -> Bool {
    lock.lock()
    defer { lock.unlock() }

    guard let pixelBuffer = pixelBufferLocked(drawBufferIndex),
          presentationTarget.isAvailable(),
          metalTextureValid else {
      return false
    }
    return presentationTarget.install(
      player: player,
      pixelBuffer: pixelBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots,
      refresh: refresh
    )
  }

  func resetNativeUploadBaseline() {
    lock.lock()
    lastPublishedNativeUploadCount = 0
    lock.unlock()
  }

  func publishRenderedTargetAndInstallNext(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int
  ) -> Bool {
    let nativeUploadCount = player.rendererOwnedPresentationUploadCount()
    lock.lock()
    defer { lock.unlock() }

    if nativeUploadCount <= lastPublishedNativeUploadCount {
      return false
    }
    guard pixelBufferLocked(drawBufferIndex) != nil else {
      return false
    }
    publishDrawBufferLocked(nativeUploadCount: nativeUploadCount)
    guard let nextDrawBuffer = pixelBufferLocked(drawBufferIndex),
          presentationTarget.isAvailable(),
          metalTextureValid else {
      return false
    }
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
    for _ in 0..<3 {
      guard let nextBuffer = makePixelBufferLocked(attributes: attributes) else {
        pixelBuffers = []
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
    lastCopiedBufferIndex = nil
    lastPublishedNativeUploadCount = 0
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

  private func publishDrawBufferLocked(nativeUploadCount: Int) {
    displayBufferIndex = drawBufferIndex
    lastPublishedNativeUploadCount = nativeUploadCount
    pixelBufferMetalUploadCount += 1
    pixelBufferReuseCount += 1
    if let displayBuffer = pixelBufferLocked(displayBufferIndex) {
      validateMetalTextureLocked(buffer: displayBuffer)
    }
    chooseNextDrawBufferLocked()
  }

  private func chooseNextDrawBufferLocked() {
    guard !pixelBuffers.isEmpty else {
      drawBufferIndex = 0
      return
    }
    for index in pixelBuffers.indices
      where index != displayBufferIndex && index != lastCopiedBufferIndex {
      drawBufferIndex = index
      return
    }
    if let index = pixelBuffers.indices.first(where: { $0 != displayBufferIndex }) {
      drawBufferIndex = index
    } else {
      drawBufferIndex = displayBufferIndex
    }
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
    let slow = totalNs >= 12_000_000 || requestNs >= 10_000_000 || result != "ok"
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
