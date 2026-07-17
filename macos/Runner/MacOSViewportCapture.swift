import Cocoa
import CoreVideo
import FlutterMacOS
import Foundation
import ImageIO
import UniformTypeIdentifiers

enum MacOSViewportCapture {
  static func capture(texture: MacOSVideoSurface?) -> Any {
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

  static func captureRegion(texture: MacOSVideoSurface?, arguments: Any?) -> Any {
    guard let texture else {
      return FlutterError(
        code: "NO_PLAYER",
        message: "No macOS Flutter texture bridge is registered",
        details: nil
      )
    }
    let x = MacOSFlutterArguments.intArg(arguments, "x") ?? 0
    let y = MacOSFlutterArguments.intArg(arguments, "y") ?? 0
    let width = MacOSFlutterArguments.intArg(arguments, "width") ?? 0
    let height = MacOSFlutterArguments.intArg(arguments, "height") ?? 0
    let maxSize = MacOSFlutterArguments.intArg(arguments, "maxSize") ?? 0
    guard width > 0, height > 0 else {
      return FlutterError(
        code: "BAD_ARGS",
        message: "width and height must be positive",
        details: nil
      )
    }

    guard let unmanagedBuffer = texture.copyPixelBuffer() else {
      return FlutterError(
        code: "CAPTURE_FAILED",
        message: "Failed to copy macOS texture pixel buffer",
        details: nil
      )
    }
    let pixelBuffer = unmanagedBuffer.takeRetainedValue()
    guard let capture = captureBgraRegion(
      buffer: pixelBuffer,
      x: x,
      y: y,
      width: width,
      height: height,
      maxSize: maxSize
    ) else {
      return FlutterError(
        code: "CAPTURE_FAILED",
        message: "Failed to capture macOS viewport region",
        details: nil
      )
    }

    var outputPath: String?
    if let path = MacOSFlutterArguments.stringArg(arguments, "outputPath"),
       !path.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
      guard let image = cgImage(
        bgra: capture.bgra,
        width: capture.width,
        height: capture.height
      ), savePng(image: image, path: path) else {
        return FlutterError(
          code: "CAPTURE_SAVE_FAILED",
          message: "Failed to save macOS viewport region PNG",
          details: nil
        )
      }
      outputPath = path
    }

    let stats = captureStats(capture.bgra)
    var map: [String: Any] = [
      "hash": fnv1a64Hex(capture.bgra),
      "width": capture.width,
      "height": capture.height,
      "avgLuma": stats.avgLuma,
      "nonBlackRatio": stats.nonBlackRatio,
    ]
    if let outputPath {
      map["outputPath"] = outputPath
    }
    return map
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

  private static func captureBgraRegion(
    buffer: CVPixelBuffer,
    x: Int,
    y: Int,
    width: Int,
    height: Int,
    maxSize: Int
  ) -> (bgra: [UInt8], width: Int, height: Int)? {
    let sourceWidth = CVPixelBufferGetWidth(buffer)
    let sourceHeight = CVPixelBufferGetHeight(buffer)
    guard sourceWidth > 0, sourceHeight > 0 else { return nil }
    let left = min(max(0, x), sourceWidth)
    let top = min(max(0, y), sourceHeight)
    let right = min(max(left, x + width), sourceWidth)
    let bottom = min(max(top, y + height), sourceHeight)
    let cropWidth = right - left
    let cropHeight = bottom - top
    guard cropWidth > 0, cropHeight > 0 else { return nil }

    var scale = 1.0
    if maxSize > 0 {
      scale = min(1.0, Double(maxSize) / Double(max(cropWidth, cropHeight)))
    }
    let outputWidth = max(1, Int((Double(cropWidth) * scale).rounded()))
    let outputHeight = max(1, Int((Double(cropHeight) * scale).rounded()))
    var output = [UInt8](repeating: 0, count: outputWidth * outputHeight * 4)

    guard CVPixelBufferLockBaseAddress(buffer, .readOnly) == kCVReturnSuccess else {
      return nil
    }
    defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
    guard let baseAddress = CVPixelBufferGetBaseAddress(buffer) else { return nil }
    let bytesPerRow = CVPixelBufferGetBytesPerRow(buffer)
    let pixelFormat = CVPixelBufferGetPixelFormatType(buffer)

    switch pixelFormat {
    case kCVPixelFormatType_32BGRA:
      guard bytesPerRow >= sourceWidth * 4 else { return nil }
      let pixels = baseAddress.assumingMemoryBound(to: UInt8.self)
      for oy in 0..<outputHeight {
        let sy = top + min(cropHeight - 1, oy * cropHeight / outputHeight)
        for ox in 0..<outputWidth {
          let sx = left + min(cropWidth - 1, ox * cropWidth / outputWidth)
          let src = sy * bytesPerRow + sx * 4
          let dst = (oy * outputWidth + ox) * 4
          output[dst + 0] = pixels[src + 0]
          output[dst + 1] = pixels[src + 1]
          output[dst + 2] = pixels[src + 2]
          output[dst + 3] = pixels[src + 3]
        }
      }
    case kCVPixelFormatType_64RGBAHalf:
      guard bytesPerRow >= sourceWidth * 8 else { return nil }
      let words = baseAddress.assumingMemoryBound(to: UInt16.self)
      let wordsPerRow = bytesPerRow / MemoryLayout<UInt16>.stride
      for oy in 0..<outputHeight {
        let sy = top + min(cropHeight - 1, oy * cropHeight / outputHeight)
        for ox in 0..<outputWidth {
          let sx = left + min(cropWidth - 1, ox * cropWidth / outputWidth)
          let src = sy * wordsPerRow + sx * 4
          let dst = (oy * outputWidth + ox) * 4
          output[dst + 0] = linearColorByte(fromHalf: words[src + 2])
          output[dst + 1] = linearColorByte(fromHalf: words[src + 1])
          output[dst + 2] = linearColorByte(fromHalf: words[src + 0])
          output[dst + 3] = alphaByte(fromHalf: words[src + 3])
        }
      }
    default:
      return nil
    }
    return (bgra: output, width: outputWidth, height: outputHeight)
  }

  private static func linearColorByte(fromHalf value: UInt16) -> UInt8 {
    let linear = clampedFinite(floatFromHalf(value), lower: 0.0, upper: 1.0)
    let encoded: Float
    if linear <= 0.0031308 {
      encoded = linear * 12.92
    } else {
      encoded = 1.055 * powf(linear, 1.0 / 2.4) - 0.055
    }
    return UInt8((clampedFinite(encoded, lower: 0.0, upper: 1.0) * 255.0).rounded())
  }

  private static func alphaByte(fromHalf value: UInt16) -> UInt8 {
    UInt8((clampedFinite(floatFromHalf(value), lower: 0.0, upper: 1.0) * 255.0).rounded())
  }

  private static func clampedFinite(_ value: Float, lower: Float, upper: Float) -> Float {
    guard value.isFinite else { return lower }
    return min(upper, max(lower, value))
  }

  private static func floatFromHalf(_ value: UInt16) -> Float {
    let sign = (UInt32(value & 0x8000)) << 16
    let exponent = Int((value & 0x7C00) >> 10)
    let mantissa = UInt32(value & 0x03FF)
    let bits: UInt32
    if exponent == 0 {
      if mantissa == 0 {
        bits = sign
      } else {
        var normalizedMantissa = mantissa
        var normalizedExponent = -14
        while (normalizedMantissa & 0x0400) == 0 {
          normalizedMantissa <<= 1
          normalizedExponent -= 1
        }
        normalizedMantissa &= 0x03FF
        bits = sign |
          (UInt32(normalizedExponent + 127) << 23) |
          (normalizedMantissa << 13)
      }
    } else if exponent == 0x1F {
      bits = sign | 0x7F800000 | (mantissa << 13)
    } else {
      bits = sign |
        (UInt32(exponent - 15 + 127) << 23) |
        (mantissa << 13)
    }
    return Float(bitPattern: bits)
  }

  private static func cgImage(bgra: [UInt8], width: Int, height: Int) -> CGImage? {
    guard width > 0, height > 0, !bgra.isEmpty else { return nil }
    let bytesPerRow = width * 4
    var mutable = bgra
    return mutable.withUnsafeMutableBytes { rawBuffer -> CGImage? in
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
        return nil
      }
      return context.makeImage()
    }
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
