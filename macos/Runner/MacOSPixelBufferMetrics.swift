import CoreVideo
import Foundation

enum MacOSPixelBufferMetrics {
  static func capture(
    buffer: CVPixelBuffer?,
    width: Int,
    height: Int,
    hashPrefix: String
  ) -> (
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
    guard let buffer else {
      return empty(width: width, height: height, hashPrefix: hashPrefix)
    }

    CVPixelBufferLockBaseAddress(buffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }

    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else {
      return empty(width: width, height: height, hashPrefix: hashPrefix)
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
      return empty(width: width, height: height, hashPrefix: hashPrefix)
    }

    let left = measureRegion(
      pixels: pixels,
      width: width,
      height: height,
      bytesPerRow: bytesPerRow,
      x0: 0,
      y0: 0,
      x1: width / 2,
      y1: height
    )
    let right = measureRegion(
      pixels: pixels,
      width: width,
      height: height,
      bytesPerRow: bytesPerRow,
      x0: width / 2,
      y0: 0,
      x1: width,
      y1: height
    )
    let lineStyle = measureOverlayLineStyle(
      pixels: pixels,
      width: width,
      height: height,
      bytesPerRow: bytesPerRow
    )

    return (
      width: Int(metrics.width),
      height: Int(metrics.height),
      avgLuma: metrics.avg_luma,
      nonBlackRatio: metrics.non_black_ratio,
      hash: String(format: "%@-%dx%d-%016llx", hashPrefix, width, height, metrics.hash),
      regionAvgLuma: [
        "left": left.avgLuma,
        "right": right.avgLuma
      ],
      regionNonBlackRatio: [
        "left": left.nonBlackRatio,
        "right": right.nonBlackRatio
      ],
      overlayLinePairedCenters: lineStyle.pairedCenters,
      overlayLineWeakWhiteCenters: lineStyle.weakWhiteCenters,
      overlayLineBlackOnlyCenters: lineStyle.blackOnlyCenters
    )
  }

  private static func measureRegion(
    pixels: UnsafePointer<UInt8>,
    width: Int,
    height: Int,
    bytesPerRow: Int,
    x0: Int,
    y0: Int,
    x1: Int,
    y1: Int
  ) -> (avgLuma: Double, nonBlackRatio: Double) {
    let clampedX0 = max(0, min(width, x0))
    let clampedY0 = max(0, min(height, y0))
    let clampedX1 = max(clampedX0, min(width, x1))
    let clampedY1 = max(clampedY0, min(height, y1))
    let sampleCount = (clampedX1 - clampedX0) * (clampedY1 - clampedY0)
    guard sampleCount > 0 else {
      return (avgLuma: 0.0, nonBlackRatio: 0.0)
    }

    var lumaSum = 0.0
    var nonBlack = 0
    for y in clampedY0..<clampedY1 {
      let row = y * bytesPerRow
      for x in clampedX0..<clampedX1 {
        let offset = row + x * 4
        let b = Double(pixels[offset])
        let g = Double(pixels[offset + 1])
        let r = Double(pixels[offset + 2])
        let luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
        lumaSum += luma
        if pixels[offset] > 8 || pixels[offset + 1] > 8 || pixels[offset + 2] > 8 {
          nonBlack += 1
        }
      }
    }

    return (
      avgLuma: lumaSum / Double(sampleCount),
      nonBlackRatio: Double(nonBlack) / Double(sampleCount)
    )
  }

  private static func empty(
    width: Int,
    height: Int,
    hashPrefix: String
  ) -> (
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
    (
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

  private static func measureOverlayLineStyle(
    pixels: UnsafePointer<UInt8>,
    width: Int,
    height: Int,
    bytesPerRow: Int
  ) -> (pairedCenters: Int, weakWhiteCenters: Int, blackOnlyCenters: Int) {
    guard width >= 6 && height >= 6 else {
      return (pairedCenters: 0, weakWhiteCenters: 0, blackOnlyCenters: 0)
    }

    func channels(_ x: Int, _ y: Int) -> (r: Int, g: Int, b: Int) {
      let offset = y * bytesPerRow + x * 4
      return (
        r: Int(pixels[offset + 2]),
        g: Int(pixels[offset + 1]),
        b: Int(pixels[offset])
      )
    }

    func luma(_ x: Int, _ y: Int) -> Int {
      let c = channels(x, y)
      return (77 * c.r + 150 * c.g + 29 * c.b) >> 8
    }

    func chroma(_ x: Int, _ y: Int) -> Int {
      let c = channels(x, y)
      return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b))
    }

    func whiteAt(_ x: Int, _ y: Int) -> Bool {
      luma(x, y) >= 190 && chroma(x, y) <= 80
    }

    func blackAt(_ x: Int, _ y: Int) -> Bool {
      luma(x, y) <= 88 && chroma(x, y) <= 70
    }

    func nearWhite(_ x: Int, _ y: Int) -> Bool {
      for dy in -2...2 {
        for dx in -2...2 {
          if (dx != 0 || dy != 0) && whiteAt(x + dx, y + dy) {
            return true
          }
        }
      }
      return false
    }

    var pairedCenters = 0
    var weakWhiteCenters = 0
    var blackOnlyCenters = 0
    for y in 2..<(height - 2) {
      for x in 2..<(width - 2) {
        if whiteAt(x, y) {
          let horizontalHalo = blackAt(x - 1, y) && blackAt(x + 1, y)
          let verticalHalo = blackAt(x, y - 1) && blackAt(x, y + 1)
          if horizontalHalo || verticalHalo {
            pairedCenters += 1
            continue
          }
          let horizontalRun = whiteAt(x - 1, y) || whiteAt(x + 1, y)
          let verticalRun = whiteAt(x, y - 1) || whiteAt(x, y + 1)
          if horizontalRun || verticalRun {
            weakWhiteCenters += 1
          }
          continue
        }

        if !blackAt(x, y) || nearWhite(x, y) {
          continue
        }
        let horizontalRun = blackAt(x - 1, y) || blackAt(x + 1, y)
        let verticalRun = blackAt(x, y - 1) || blackAt(x, y + 1)
        if horizontalRun || verticalRun {
          blackOnlyCenters += 1
        }
      }
    }
    return (
      pairedCenters: pairedCenters,
      weakWhiteCenters: weakWhiteCenters,
      blackOnlyCenters: blackOnlyCenters
    )
  }
}
