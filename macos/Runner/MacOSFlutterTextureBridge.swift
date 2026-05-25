import Cocoa
import CoreVideo
import FlutterMacOS

final class MacOSFlutterTextureBridge: NSObject, FlutterTexture {
  private let lock = NSLock()
  private(set) var width: Int
  private(set) var height: Int
  private let isSyntheticSource: Bool
  private var nativeMetalPresentationBackend: OpaquePointer?
  private let hashPrefix: String
  private var pixelBuffer: CVPixelBuffer?
  private var pixelBufferBackBuffer: CVPixelBuffer?
  private var pixelBufferRetiredBuffer: CVPixelBuffer?
  private var pixelBufferRebuildCount = 0
  private var pixelBufferReuseCount = 0
  private var pixelBufferMetalUploadCount = 0
  private var pixelBufferMetalUploadFailureCount = 0
  private var metalTextureValid = false
  private var metalTextureCreationCount = 0
  private var metalTextureFailureCount = 0
  private var metalTextureLastError = ""

  init(width: Int, height: Int) {
    self.width = width
    self.height = height
    self.isSyntheticSource = true
    self.hashPrefix = "macos-synthetic"
    super.init()
    createNativeMetalPresentationBackend()
    rebuildPixelBuffer()
  }

  init(nativeWidth: Int, nativeHeight: Int) {
    self.width = nativeWidth
    self.height = nativeHeight
    self.isSyntheticSource = false
    self.hashPrefix = "macos-native-frame"
    super.init()
    createNativeMetalPresentationBackend()
    rebuildPixelBuffer()
  }

  deinit {
    if let nativeMetalPresentationBackend {
      VPMacOSMetalPresentationBackendDestroy(nativeMetalPresentationBackend)
    }
  }

  func resize(width: Int, height: Int) -> Bool {
    lock.lock()
    defer { lock.unlock() }

    guard width != self.width || height != self.height else { return false }
    self.width = width
    self.height = height
    createNativeMetalPresentationBackendLocked()
    rebuildPixelBufferLocked()
    return true
  }

  func updateFromNativePlayer(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int,
    waitTimeoutMs: Int
  ) throws -> MacOSNativeFrameInfo {
    lock.lock()
    defer { lock.unlock() }

    guard !isSyntheticSource else {
      throw MacOSNativePlayerError.invalidPayload
    }
    if pixelBuffer == nil {
      rebuildPixelBufferLocked()
    }
    guard let pixelBuffer else {
      throw MacOSNativePlayerError.invalidPayload
    }

    guard let info = try copyFromNativePlayerWithMetalUpload(
      player,
      pixelBuffer: pixelBuffer,
      maxTrackSlots: maxTrackSlots,
      waitTimeoutMs: waitTimeoutMs
    ) else {
      throw MacOSNativePlayerError.failed("renderer-owned Metal presentation is unavailable")
    }
    pixelBufferReuseCount += 1
    return info
  }

  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    lock.lock()
    defer { lock.unlock() }

    guard let pixelBuffer else { return nil }
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
    hash: String
  ) {
    lock.lock()
    defer { lock.unlock() }

    guard let pixelBuffer else {
      return (
        width: width,
        height: height,
        avgLuma: 0.0,
        nonBlackRatio: 0.0,
        hash: "\(hashPrefix)-empty"
      )
    }
    return measure(buffer: pixelBuffer)
  }

  func diagnostics() -> (
    rebuildCount: Int,
    reuseCount: Int,
    metalUploadCount: Int,
    metalYuvUploadCount: Int,
    metalCVPixelBufferUploadCount: Int,
    metalUploadFailureCount: Int,
    presentationUploadMode: String,
    presentPackageUploadCount: Int,
    presentPackageCopyUs: Int,
    presentPackageGpuWaitUs: Int,
    presentPackageTotalUs: Int,
    presentPackageStorage: String,
    metalAvailable: Bool,
    metalTextureCacheAvailable: Bool,
    metalTextureValid: Bool,
    metalTextureCreationCount: Int,
    metalTextureFailureCount: Int,
    metalTextureLastError: String
  ) {
    lock.lock()
    defer { lock.unlock() }

    return (
      rebuildCount: pixelBufferRebuildCount,
      reuseCount: pixelBufferReuseCount,
      metalUploadCount: max(
        pixelBufferMetalUploadCount,
        nativeMetalPresentPackageUploadCountLocked()
      ),
      metalYuvUploadCount: nativeMetalUploaderDirectYuvUploadCountLocked(),
      metalCVPixelBufferUploadCount: nativeMetalUploaderCVPixelBufferUploadCountLocked(),
      metalUploadFailureCount: pixelBufferMetalUploadFailureCount,
      presentationUploadMode: presentationUploadModeLocked(),
      presentPackageUploadCount: nativeMetalPresentPackageUploadCountLocked(),
      presentPackageCopyUs: nativeMetalLastPresentPackageCopyUsLocked(),
      presentPackageGpuWaitUs: nativeMetalLastPresentPackageGpuWaitUsLocked(),
      presentPackageTotalUs: nativeMetalLastPresentPackageTotalUsLocked(),
      presentPackageStorage: nativeMetalLastPresentPackageStorageLocked(),
      metalAvailable: nativeMetalUploaderAvailableLocked(),
      metalTextureCacheAvailable: nativeMetalUploaderAvailableLocked(),
      metalTextureValid: metalTextureValid,
      metalTextureCreationCount: metalTextureCreationCount,
      metalTextureFailureCount: metalTextureFailureCount,
      metalTextureLastError: metalTextureLastError
    )
  }

  func installNativePresentationTarget(
    _ player: MacOSNativePlayerSession,
    maxTrackSlots: Int
  ) -> Bool {
    lock.lock()
    defer { lock.unlock() }

    guard !isSyntheticSource,
          let nativeMetalPresentationBackend,
          let pixelBuffer,
          nativeMetalUploaderAvailableLocked(),
          metalTextureValid else {
      return false
    }
    return player.setMetalPresentationTarget(
      backend: nativeMetalPresentationBackend,
      pixelBuffer: pixelBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots
    )
  }

  private func rebuildPixelBuffer() {
    lock.lock()
    defer { lock.unlock() }

    rebuildPixelBufferLocked()
  }

  private func rebuildPixelBufferLocked() {
    let attributes = [
      kCVPixelBufferCGImageCompatibilityKey as String: true,
      kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
      kCVPixelBufferMetalCompatibilityKey as String: true,
      kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ] as CFDictionary

    guard let nextBuffer = makePixelBufferLocked(attributes: attributes) else {
      pixelBuffer = nil
      pixelBufferBackBuffer = nil
      pixelBufferRetiredBuffer = nil
      return
    }
    pixelBufferRebuildCount += 1

    fill(buffer: nextBuffer)
    pixelBuffer = nextBuffer
    pixelBufferBackBuffer = makePixelBufferLocked(attributes: attributes)
    pixelBufferRetiredBuffer = makePixelBufferLocked(attributes: attributes)
    validateMetalTextureLocked(buffer: nextBuffer)
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

  private func makePixelBufferLocked() -> CVPixelBuffer? {
    let attributes = [
      kCVPixelBufferCGImageCompatibilityKey as String: true,
      kCVPixelBufferCGBitmapContextCompatibilityKey as String: true,
      kCVPixelBufferMetalCompatibilityKey as String: true,
      kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ] as CFDictionary
    return makePixelBufferLocked(attributes: attributes)
  }

  private func ensureBackPixelBufferLocked() -> CVPixelBuffer? {
    if let pixelBufferBackBuffer,
       CVPixelBufferGetWidth(pixelBufferBackBuffer) == width,
       CVPixelBufferGetHeight(pixelBufferBackBuffer) == height {
      return pixelBufferBackBuffer
    }
    pixelBufferBackBuffer = makePixelBufferLocked()
    return pixelBufferBackBuffer
  }

  private func publishBackBufferLocked(_ buffer: CVPixelBuffer) {
    let previousFront = pixelBuffer
    let previousRetired = pixelBufferRetiredBuffer
    pixelBuffer = buffer
    pixelBufferRetiredBuffer = previousFront
    if let previousFront,
       CVPixelBufferGetWidth(previousFront) != width ||
       CVPixelBufferGetHeight(previousFront) != height {
      pixelBufferRetiredBuffer = nil
    }
    if let previousRetired,
       CVPixelBufferGetWidth(previousRetired) == width,
       CVPixelBufferGetHeight(previousRetired) == height {
      pixelBufferBackBuffer = previousRetired
    } else {
      pixelBufferBackBuffer = makePixelBufferLocked()
    }
  }

  private func createNativeMetalPresentationBackend() {
    lock.lock()
    defer { lock.unlock() }
    createNativeMetalPresentationBackendLocked()
  }

  private func createNativeMetalPresentationBackendLocked() {
    if let nativeMetalPresentationBackend {
      VPMacOSMetalPresentationBackendDestroy(nativeMetalPresentationBackend)
    }
    nativeMetalPresentationBackend = VPMacOSMetalPresentationBackendCreate(
      Int32(width),
      Int32(height)
    )
  }

  private func nativeMetalUploaderAvailableLocked() -> Bool {
    guard let nativeMetalPresentationBackend else { return false }
    return VPMacOSMetalPresentationBackendIsAvailable(nativeMetalPresentationBackend) != 0
  }

  private func nativeMetalUploaderDirectYuvUploadCountLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendDirectYUVUploadCount(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalUploaderCVPixelBufferUploadCountLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendCVPixelBufferUploadCount(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalPresentPackageUploadCountLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendPresentPackageUploadCount(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalLastPresentPackageCopyUsLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendLastPresentPackageCopyUs(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalLastPresentPackageGpuWaitUsLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendLastPresentPackageGpuWaitUs(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalLastPresentPackageTotalUsLocked() -> Int {
    guard let nativeMetalPresentationBackend else { return 0 }
    return Int(
      VPMacOSMetalPresentationBackendLastPresentPackageTotalUs(nativeMetalPresentationBackend)
    )
  }

  private func nativeMetalLastPresentPackageStorageLocked() -> String {
    guard let nativeMetalPresentationBackend else { return "unavailable" }
    let storage = VPMacOSMetalPresentationBackendLastPresentPackageStorage(
      nativeMetalPresentationBackend
    )
    switch storage {
    case Int32(VPMacOSNativePresentPackageStorageYUV):
      return "yuv"
    case Int32(VPMacOSNativePresentPackageStorageBGRA):
      return "bgra"
    case Int32(VPMacOSNativePresentPackageStorageCVPixelBuffer):
      return "cvpixelbuffer"
    default:
      return "unavailable"
    }
  }

  private func presentationUploadModeLocked() -> String {
    if metalTextureValid, nativeMetalUploaderAvailableLocked() {
      switch nativeMetalLastPresentPackageStorageLocked() {
      case "cvpixelbuffer":
        return "metal-cvpixelbuffer-present-package"
      case "yuv":
        return "metal-yuv-present-package"
      case "bgra":
        return "metal-bgra-present-package"
      default:
        return "metal-presentation-target-ready"
      }
    }
    if pixelBuffer != nil {
      return "metal-presentation-target-unavailable"
    }
    return "unavailable"
  }

  private func copyFromNativePlayerWithMetalUpload(
    _ player: MacOSNativePlayerSession,
    pixelBuffer: CVPixelBuffer,
    maxTrackSlots: Int,
    waitTimeoutMs: Int
  ) throws -> MacOSNativeFrameInfo? {
    guard let nativeMetalPresentationBackend,
          VPMacOSMetalPresentationBackendIsAvailable(nativeMetalPresentationBackend) != 0 else {
      throw MacOSNativePlayerError.failed("renderer-owned Metal presentation backend is unavailable")
    }
    do {
      guard player.setMetalPresentationTarget(
        backend: nativeMetalPresentationBackend,
        pixelBuffer: pixelBuffer,
        width: width,
        height: height,
        maxTrackSlots: maxTrackSlots
      ) else {
        throw MacOSNativePlayerError.failed("failed to install renderer-owned Metal presentation target")
      }
      let deadline = Date().addingTimeInterval(Double(waitTimeoutMs) / 1000.0)
      var lastError: Error?
      repeat {
        do {
          let info = try player.presentCurrentFrameToMetalTarget()
          pixelBufferMetalUploadCount += 1
          validateMetalTextureLocked(buffer: pixelBuffer)
          return info
        } catch {
          lastError = error
          guard (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable == true,
                Date() < deadline else {
            throw error
          }
          Thread.sleep(forTimeInterval: 0.01)
        }
      } while Date() < deadline
      if let lastError {
        throw lastError
      }
      return nil
    } catch {
      if (error as? MacOSNativePlayerError)?.isTransientFrameUnavailable != true {
        pixelBufferMetalUploadFailureCount += 1
        NSLog("VoidPlayer macOS renderer-owned Metal refresh failed: \(error)")
      }
      throw error
    }
  }

  private func validateMetalTextureLocked(buffer: CVPixelBuffer) {
    guard let nativeMetalPresentationBackend else {
      metalTextureValid = false
      metalTextureLastError = "native Metal presentation backend is null"
      return
    }

    var error = [CChar](repeating: 0, count: 512)
    let status = VPMacOSMetalPresentationBackendValidatePixelBufferChecked(
      nativeMetalPresentationBackend,
      UnsafeMutableRawPointer(Unmanaged.passUnretained(buffer).toOpaque()),
      Int32(width),
      Int32(height),
      &error,
      error.count
    )
    if status == 0 {
      metalTextureCreationCount += 1
      metalTextureValid = true
      metalTextureLastError = ""
    } else {
      metalTextureFailureCount += 1
      metalTextureValid = false
      metalTextureLastError = String(cString: error)
    }
  }

  private func measure(buffer: CVPixelBuffer) -> (
    width: Int,
    height: Int,
    avgLuma: Double,
    nonBlackRatio: Double,
    hash: String
  ) {
    CVPixelBufferLockBaseAddress(buffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }

    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else {
      return (
        width: width,
        height: height,
        avgLuma: 0.0,
        nonBlackRatio: 0.0,
        hash: "\(hashPrefix)-empty"
      )
    }

    let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
    let pixels = baseAddress.assumingMemoryBound(to: UInt8.self)
    var metrics = VPMacOSCaptureMetrics()
    let ret = VPMacOSMeasureBGRA(
      pixels,
      Int32(width),
      Int32(height),
      Int32(bytesPerRow),
      &metrics
    )
    guard ret == 0 else {
      return (
        width: width,
        height: height,
        avgLuma: 0.0,
        nonBlackRatio: 0.0,
        hash: "\(hashPrefix)-empty"
      )
    }

    return (
      width: Int(metrics.width),
      height: Int(metrics.height),
      avgLuma: metrics.avg_luma,
      nonBlackRatio: metrics.non_black_ratio,
      hash: String(format: "%@-%dx%d-%016llx", hashPrefix, width, height, metrics.hash)
    )
  }

  private func fill(buffer: CVPixelBuffer) {
    CVPixelBufferLockBaseAddress(buffer, [])
    defer { CVPixelBufferUnlockBaseAddress(buffer, []) }

    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else { return }
    let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
    let pixels = baseAddress.assumingMemoryBound(to: UInt8.self)
    let barWidth = max(1, width / 6)

    for y in 0..<height {
      for x in 0..<width {
        let bar = min(5, x / barWidth)
        let gradient = UInt8((x * 63) / max(1, width - 1))
        let scanline = UInt8((y * 31) / max(1, height - 1))
        let color = colorForBar(bar, gradient: gradient, scanline: scanline)
        let offset = y * bytesPerRow + x * 4
        pixels[offset + 0] = color.b
        pixels[offset + 1] = color.g
        pixels[offset + 2] = color.r
        pixels[offset + 3] = 255
      }
    }
  }

  private func copyBGRA(_ data: Data, to buffer: CVPixelBuffer) {
    CVPixelBufferLockBaseAddress(buffer, [])
    defer { CVPixelBufferUnlockBaseAddress(buffer, []) }

    copyBGRALocked(data, to: buffer)
  }

  private func copyBGRALocked(_ data: Data, to buffer: CVPixelBuffer) {
    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else { return }
    let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
    let rowBytes = width * 4

    data.withUnsafeBytes { rawSource in
      guard let source = rawSource.baseAddress else { return }
      for y in 0..<height {
        let sourceOffset = y * rowBytes
        guard sourceOffset < rawSource.count else { break }
        let sourceRowBytes = min(rowBytes, rawSource.count - sourceOffset)
        memcpy(
          baseAddress.advanced(by: y * bytesPerRow),
          source.advanced(by: sourceOffset),
          sourceRowBytes
        )
      }
    }
  }

  private func colorForBar(
    _ bar: Int,
    gradient: UInt8,
    scanline: UInt8
  ) -> (b: UInt8, g: UInt8, r: UInt8) {
    switch bar {
    case 0:
      return (32, 48 &+ scanline, 220)
    case 1:
      return (32 &+ gradient, 180, 64)
    case 2:
      return (220, 180 &- min(scanline, 120), 48)
    case 3:
      return (180, 64 &+ gradient, 200)
    case 4:
      return (64, 210, 210 &- min(gradient, 160))
    default:
      return (200 &- min(scanline, 120), 88, 96 &+ gradient)
    }
  }
}
