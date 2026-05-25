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
    return MacOSPixelBufferMetrics.capture(
      buffer: pixelBuffer,
      width: width,
      height: height,
      hashPrefix: hashPrefix
    )
  }

  func diagnostics() -> (
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
  ) {
    lock.lock()
    defer { lock.unlock() }

    return (
      rebuildCount: pixelBufferRebuildCount,
      reuseCount: pixelBufferReuseCount,
      metalUploadCount: pixelBufferMetalUploadCount,
      metalUploadFailureCount: pixelBufferMetalUploadFailureCount,
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
      return
    }
    pixelBufferRebuildCount += 1

    MacOSSyntheticTexturePattern.fill(buffer: nextBuffer, width: width, height: height)
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

}
