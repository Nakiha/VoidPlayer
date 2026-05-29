import CoreVideo
import FlutterMacOS

final class MacOSSyntheticTextureBridge: NSObject, MacOSVideoTexture {
  private let lock = NSLock()
  private(set) var width: Int
  private(set) var height: Int
  private var pixelBuffer: CVPixelBuffer?
  private var rebuildCount = 0
  private var reuseCount = 0

  init(width: Int, height: Int) {
    self.width = width
    self.height = height
    super.init()
    rebuildPixelBuffer()
  }

  func resize(width: Int, height: Int) -> Bool {
    lock.lock()
    defer { lock.unlock() }

    guard width != self.width || height != self.height else { return false }
    self.width = width
    self.height = height
    rebuildPixelBufferLocked()
    return true
  }

  func copyPixelBuffer() -> Unmanaged<CVPixelBuffer>? {
    lock.lock()
    defer { lock.unlock() }

    guard let pixelBuffer else { return nil }
    reuseCount += 1
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

    guard let pixelBuffer else {
      return (
        width: width,
        height: height,
        avgLuma: 0.0,
        nonBlackRatio: 0.0,
        hash: "macos-synthetic-empty",
        regionAvgLuma: [:],
        regionNonBlackRatio: [:]
      )
    }
    return MacOSPixelBufferMetrics.capture(
      buffer: pixelBuffer,
      width: width,
      height: height,
      hashPrefix: "macos-synthetic"
    )
  }

  func diagnostics() -> MacOSTextureDiagnostics {
    lock.lock()
    defer { lock.unlock() }

    return (
      rebuildCount: rebuildCount,
      reuseCount: reuseCount,
      metalUploadCount: 0,
      metalUploadFailureCount: 0,
      metalAvailable: false,
      metalTextureCacheAvailable: false,
      metalTextureValid: false,
      metalTextureCreationCount: 0,
      metalTextureFailureCount: 0,
      metalTextureLastError: "",
      inFlightMetalBufferCount: 0,
      metalBufferExhaustionCount: 0
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
      kCVPixelBufferIOSurfacePropertiesKey as String: [:],
    ] as CFDictionary
    var nextBuffer: CVPixelBuffer?
    let status = CVPixelBufferCreate(
      kCFAllocatorDefault,
      width,
      height,
      kCVPixelFormatType_32BGRA,
      attributes,
      &nextBuffer
    )
    guard status == kCVReturnSuccess, let nextBuffer else {
      pixelBuffer = nil
      return
    }
    rebuildCount += 1
    MacOSSyntheticTexturePattern.fill(buffer: nextBuffer, width: width, height: height)
    pixelBuffer = nextBuffer
  }
}

enum MacOSSyntheticTexturePattern {
  static func clear(buffer: CVPixelBuffer, width: Int, height: Int) {
    CVPixelBufferLockBaseAddress(buffer, [])
    defer { CVPixelBufferUnlockBaseAddress(buffer, []) }

    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else { return }
    let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
    let pixels = baseAddress.assumingMemoryBound(to: UInt8.self)

    for y in 0..<height {
      let row = pixels.advanced(by: y * bytesPerRow)
      row.update(repeating: 0, count: width * 4)
    }
  }

  static func fill(buffer: CVPixelBuffer, width: Int, height: Int) {
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

  private static func colorForBar(
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
