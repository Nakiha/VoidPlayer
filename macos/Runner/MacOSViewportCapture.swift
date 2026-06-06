import Cocoa
import FlutterMacOS
import Foundation
import ImageIO
import UniformTypeIdentifiers

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

  static func captureWindow(arguments: Any?) -> Any {
    guard let window = NSApp.keyWindow ?? NSApp.mainWindow ?? NSApp.windows.first else {
      return FlutterError(
        code: "NO_WINDOW",
        message: "No macOS window is available",
        details: nil
      )
    }
    let windowId = CGWindowID(window.windowNumber)
    guard let image = CGWindowListCreateImage(
      .null,
      .optionIncludingWindow,
      windowId,
      [.boundsIgnoreFraming]
    ) else {
      return FlutterError(
        code: "CAPTURE_FAILED",
        message: "Failed to capture macOS window",
        details: nil
      )
    }

    let width = image.width
    let height = image.height
    guard let bgra = bgraBytes(from: image) else {
      return FlutterError(
        code: "CAPTURE_FAILED",
        message: "Failed to read macOS window pixels",
        details: nil
      )
    }

    var outputPath: String?
    if let path = (arguments as? [String: Any])?["outputPath"] as? String,
       !path.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
      guard savePng(image: image, path: path) else {
        return FlutterError(
          code: "CAPTURE_SAVE_FAILED",
          message: "Failed to save macOS window PNG",
          details: nil
        )
      }
      outputPath = path
    }

    let stats = captureStats(bgra)
    var map: [String: Any] = [
      "hash": fnv1a64Hex(bgra),
      "width": width,
      "height": height,
      "avgLuma": stats.avgLuma,
      "nonBlackRatio": stats.nonBlackRatio,
    ]
    if let outputPath {
      map["outputPath"] = outputPath
    }
    return map
  }

  private static func bgraBytes(from image: CGImage) -> [UInt8]? {
    let width = image.width
    let height = image.height
    guard width > 0, height > 0 else { return nil }
    let bytesPerRow = width * 4
    var bytes = [UInt8](repeating: 0, count: bytesPerRow * height)
    let ok = bytes.withUnsafeMutableBytes { rawBuffer -> Bool in
      guard let baseAddress = rawBuffer.baseAddress,
            let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
            let context = CGContext(
              data: baseAddress,
              width: width,
              height: height,
              bitsPerComponent: 8,
              bytesPerRow: bytesPerRow,
              space: colorSpace,
              bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue |
                CGBitmapInfo.byteOrder32Little.rawValue
            ) else {
        return false
      }
      context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
      return true
    }
    return ok ? bytes : nil
  }

  private static func captureStats(_ bgra: [UInt8]) -> (avgLuma: Double, nonBlackRatio: Double) {
    let pixelCount = bgra.count / 4
    guard pixelCount > 0 else { return (0, 0) }
    var lumaSum: UInt64 = 0
    var nonBlack = 0
    for index in stride(from: 0, to: bgra.count, by: 4) {
      let b = Int(bgra[index])
      let g = Int(bgra[index + 1])
      let r = Int(bgra[index + 2])
      let luma = (77 * r + 150 * g + 29 * b) >> 8
      lumaSum += UInt64(luma)
      if r > 8 || g > 8 || b > 8 {
        nonBlack += 1
      }
    }
    return (
      Double(lumaSum) / Double(pixelCount),
      Double(nonBlack) / Double(pixelCount)
    )
  }

  private static func fnv1a64Hex(_ bytes: [UInt8]) -> String {
    var hash: UInt64 = 14_695_981_039_346_656_037
    for byte in bytes {
      hash ^= UInt64(byte)
      hash = hash &* 1_099_511_628_211
    }
    return String(format: "%016llx", hash)
  }

  private static func savePng(image: CGImage, path: String) -> Bool {
    let resolvedPath: String
    if (path as NSString).isAbsolutePath {
      resolvedPath = path
    } else {
      resolvedPath = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        .appendingPathComponent(path)
        .path
    }
    let url = URL(fileURLWithPath: resolvedPath)
    do {
      try FileManager.default.createDirectory(
        at: url.deletingLastPathComponent(),
        withIntermediateDirectories: true
      )
      guard let destination = CGImageDestinationCreateWithURL(
        url as CFURL,
        UTType.png.identifier as CFString,
        1,
        nil
      ) else {
        return false
      }
      CGImageDestinationAddImage(destination, image, nil)
      return CGImageDestinationFinalize(destination)
    } catch {
      return false
    }
  }
}
