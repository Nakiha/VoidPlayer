import Foundation
import ImageIO
import UniformTypeIdentifiers

enum MacOSMediaInputGuard {
  static func unsupportedReason(path: String) -> String? {
    let url = URL(fileURLWithPath: path)
    if let type = UTType(filenameExtension: url.pathExtension),
       isUnsupportedHeifType(type) {
      return unsupportedHeifMessage
    }
    guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
          let typeIdentifier = CGImageSourceGetType(source) as String?,
          let type = UTType(typeIdentifier),
          isUnsupportedHeifType(type) else {
      return nil
    }
    return unsupportedHeifMessage
  }

  private static var unsupportedHeifMessage: String {
    "HEIC/HEIF images are not supported by the native renderer yet"
  }

  private static func isUnsupportedHeifType(_ type: UTType) -> Bool {
    if type.conforms(to: .heic) {
      return true
    }
    let identifier = type.identifier.lowercased()
    return identifier.contains("heic") || identifier.contains("heif")
  }
}
