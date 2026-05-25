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
    hash: String
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
    maxTrackSlots: Int
  ) -> Bool {
    guard let backend, isAvailable() else { return false }
    return player.setMetalPresentationTarget(
      backend: backend,
      pixelBuffer: pixelBuffer,
      width: width,
      height: height,
      maxTrackSlots: maxTrackSlots
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
  private var pixelBuffer: CVPixelBuffer?
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
    maxTrackSlots: Int
  ) -> Bool {
    lock.lock()
    defer { lock.unlock() }

    guard let pixelBuffer,
          presentationTarget.isAvailable(),
          metalTextureValid else {
      return false
    }
    return presentationTarget.install(
      player: player,
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
      return
    }
    pixelBufferRebuildCount += 1

    MacOSSyntheticTexturePattern.clear(buffer: nextBuffer, width: width, height: height)
    pixelBuffer = nextBuffer
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

  private func copyFromNativePlayerWithMetalUpload(
    _ player: MacOSNativePlayerSession,
    pixelBuffer: CVPixelBuffer,
    maxTrackSlots: Int,
    waitTimeoutMs: Int
  ) throws -> MacOSNativeFrameInfo? {
    guard presentationTarget.isAvailable() else {
      throw MacOSNativePlayerError.failed("renderer-owned Metal presentation backend is unavailable")
    }
    do {
      guard presentationTarget.install(
        player: player,
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

}
