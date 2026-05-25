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
    hash: String
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

    return (
      width: Int(metrics.width),
      height: Int(metrics.height),
      avgLuma: metrics.avg_luma,
      nonBlackRatio: metrics.non_black_ratio,
      hash: String(format: "%@-%dx%d-%016llx", hashPrefix, width, height, metrics.hash)
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
    hash: String
  ) {
    (
      width: width,
      height: height,
      avgLuma: 0.0,
      nonBlackRatio: 0.0,
      hash: "\(hashPrefix)-empty"
    )
  }
}
