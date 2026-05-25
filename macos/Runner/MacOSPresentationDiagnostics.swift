import Foundation

enum MacOSPresentationDiagnostics {
  static func fallbackReason(
    player: MacOSNativePlayerSession?,
    targetInstalled: Bool,
    perfStats: [String: Any]?
  ) -> String {
    guard let player else {
      return "no-native-player"
    }
    if !player.hardwareDecodeActive() {
      return "software-decode"
    }
    if player.hardwareDecodeDownloadsToCpu() {
      return "hardware-download-to-cpu"
    }
    if !targetInstalled {
      return "native-presentation-target-unavailable"
    }
    let uploadCount = int64Diagnostic(perfStats?["rendererOwnedUploadCount"])
    let failureCount = int64Diagnostic(perfStats?["rendererOwnedUploadFailureCount"])
    if uploadCount == 0 && failureCount > 0 {
      return "renderer-owned-upload-failed"
    }
    return "none"
  }

  static func uploadMode(
    perfStats: [String: Any]?,
    targetReady: Bool,
    targetInstalled: Bool,
    textureRegistered: Bool
  ) -> String {
    switch perfStats?["rendererOwnedPresentPackageStorage"] as? String {
    case "cvpixelbuffer":
      return "metal-cvpixelbuffer-present-package"
    case "yuv":
      return "metal-yuv-present-package"
    case "bgra":
      return "metal-bgra-present-package"
    default:
      break
    }
    if targetReady && targetInstalled {
      return "metal-presentation-target-ready"
    }
    if textureRegistered {
      return "metal-presentation-target-unavailable"
    }
    return "unavailable"
  }

  private static func int64Diagnostic(_ value: Any?) -> Int64 {
    switch value {
    case let value as Int64:
      return value
    case let value as Int:
      return Int64(value)
    case let value as UInt64:
      return Int64(min(value, UInt64(Int64.max)))
    case let value as Double:
      return Int64(value)
    default:
      return 0
    }
  }
}
