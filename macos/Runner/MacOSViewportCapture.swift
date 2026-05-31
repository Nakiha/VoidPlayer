import FlutterMacOS
import Foundation

enum MacOSViewportCapture {
  static func capture(texture: MacOSVideoTexture?) -> Any {
    guard let texture else {
      return FlutterError(
        code: "NO_PLAYER",
        message: "No macOS Flutter texture bridge is registered",
        details: nil
      )
    }
    let metrics = texture.captureMetrics()
    return [
      "hash": metrics.hash,
      "width": metrics.width,
      "height": metrics.height,
      "avgLuma": metrics.avgLuma,
      "nonBlackRatio": metrics.nonBlackRatio,
      "regionAvgLuma": metrics.regionAvgLuma,
      "regionNonBlackRatio": metrics.regionNonBlackRatio,
      "overlayLinePairedCenters": metrics.overlayLinePairedCenters,
      "overlayLineWeakWhiteCenters": metrics.overlayLineWeakWhiteCenters,
      "overlayLineBlackOnlyCenters": metrics.overlayLineBlackOnlyCenters,
    ]
  }
}
