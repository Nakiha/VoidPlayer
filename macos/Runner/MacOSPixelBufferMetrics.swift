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
    regionNonBlackRatio: [String: Double]
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
      ]
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
    regionNonBlackRatio: [String: Double]
  ) {
    (
      width: width,
      height: height,
      avgLuma: 0.0,
      nonBlackRatio: 0.0,
      hash: "\(hashPrefix)-empty",
      regionAvgLuma: [:],
      regionNonBlackRatio: [:]
    )
  }
}
