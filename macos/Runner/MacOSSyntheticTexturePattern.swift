import CoreVideo

enum MacOSSyntheticTexturePattern {
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
